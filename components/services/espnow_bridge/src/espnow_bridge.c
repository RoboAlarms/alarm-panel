/* CP-003: ESP-NOW on the radio, RPC to the P4. Wi-Fi callbacks only copy to
 * bounded queues. Hosted RPC and esp_now sends run on our worker, never there.
 * SEC-012 (docs/bugs/B033, F7): anyone in radio range can send ESP-NOW frames, and each one the C6
 * passes on loads the SDIO link the P4's Wi-Fi shares. A peer is known only when the P4 names it
 * (ENB_ADD: the P4 names the peers it sends sealed frames to, which only paired ones get), never
 * because a frame went to it, so a stranger answered once stays a stranger. Each known peer has
 * its own bucket (a spoofed keypad address can only crowd out that keypad); everyone else (a
 * keypad being paired, or a stranger) shares one small bucket. */
#include "espnow_bridge.h"
#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED_CP
#include "alarm_proto/espnow_bridge_proto.h"
#include "eh_cp_feat_peer_data.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

typedef struct {
    size_t n;
    uint8_t p[ENB_MAX];
} packet_t;
static QueueHandle_t commands, bulk, received, completed;
static bool ready;
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};

/* The callbacks' packets are static, not on their callers' small stacks: rx runs only on the
 * Wi-Fi task and request only on Hosted's receive task (each one context, never re-entered),
 * and xQueueSend copies the packet before the next call. */
static packet_t rx_packet, request_packet;

/* The peers the P4 named (ENB_ADD), read by rx() on the Wi-Fi task and written by the worker:
 * under known_lock. The broadcast address is never one; a full table drops its oldest. */
#define KNOWN_MAX       20
#define KNOWN_BURST     48u
#define KNOWN_PER_S     200u
#define STRANGER_BURST  20u
#define STRANGER_PER_S  20u
typedef struct {
    uint32_t tokens;
    int64_t at_us;
} bucket_t;
typedef struct {
    uint8_t mac[6];
    bucket_t bucket;
} known_t;
static portMUX_TYPE known_lock = portMUX_INITIALIZER_UNLOCKED;
static known_t known[KNOWN_MAX];
static size_t known_n;
static bucket_t stranger_bucket = {STRANGER_BURST, 0};

static bool take(bucket_t *b, uint32_t burst, uint32_t per_s, int64_t now)
{
    const int64_t every = 1000000 / per_s;
    if (b->tokens < burst && now > b->at_us) {
        const int64_t gained = (now - b->at_us) / every;
        b->tokens = gained >= (int64_t)(burst - b->tokens) ? burst : b->tokens + (uint32_t)gained;
        b->at_us = b->tokens >= burst ? now : b->at_us + gained * every;
    } else if (b->tokens >= burst) {
        b->at_us = now;
    }
    if (b->tokens == 0u)
        return false;
    b->tokens--;
    return true;
}

static known_t *known_of(const uint8_t *mac)
{
    for (size_t i = 0; i < known_n; i++)
        if (memcmp(known[i].mac, mac, 6) == 0)
            return &known[i];
    return NULL;
}

static void known_set(const uint8_t *mac, bool add)
{
    if (memcmp(mac, broadcast, 6) == 0)
        return;
    portENTER_CRITICAL(&known_lock);
    known_t *k = known_of(mac);
    if (k != NULL && !add) {
        const size_t i = (size_t)(k - known);
        memmove(&known[i], &known[i + 1], (known_n - i - 1) * sizeof(known[0]));
        known_n--;
    } else if (k == NULL && add) {
        if (known_n == KNOWN_MAX) {
            memmove(&known[0], &known[1], (KNOWN_MAX - 1) * sizeof(known[0]));
            known_n--;
        }
        memcpy(known[known_n].mac, mac, 6);
        known[known_n].bucket = (bucket_t){KNOWN_BURST, esp_timer_get_time()};
        known_n++;
    }
    portEXIT_CRITICAL(&known_lock);
}

static void rx(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !data || len <= 0 || len > ENB_DATA_MAX)
        return;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&known_lock);
    known_t *k = known_of(info->src_addr);
    const bool pass = k != NULL ? take(&k->bucket, KNOWN_BURST, KNOWN_PER_S, now)
                                : take(&stranger_bucket, STRANGER_BURST, STRANGER_PER_S, now);
    portEXIT_CRITICAL(&known_lock);
    if (!pass)
        return;
    packet_t *const p = &rx_packet;
    memset(p->p, 0, ENB_HEADER);
    p->n = ENB_HEADER + (size_t)len;
    p->p[0] = ENB_VERSION;
    p->p[1] = ENB_RECV;
    memcpy(p->p + 4, info->src_addr, 6);
    if (info->rx_ctrl) {
        p->p[10] = info->rx_ctrl->channel;
        p->p[11] = (uint8_t)info->rx_ctrl->rssi;
    }
    enb_put16(p->p + 12, (uint16_t)len);
    memcpy(p->p + ENB_HEADER, data, (size_t)len);
    (void)xQueueSend(received, p, 0);
}

static void sent(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    (void)xQueueSend(completed, &status, 0);
}

static void request(uint32_t id, const uint8_t *data, size_t n, void *ctx)
{
    (void)id;
    (void)ctx;
    if (!enb_valid(data, n) || data[1] > ENB_SEND)
        return;
    request_packet.n = n;
    memcpy(request_packet.p, data, n);
    QueueHandle_t q = data[1] == ENB_SEND && data[11] == 0 ? bulk : commands;
    (void)xQueueSend(q, &request_packet, 0); /* the link protocol retries a full queue */
}

static esp_err_t peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
        return ESP_OK;
    esp_now_peer_info_t p = {.channel = 0, .ifidx = WIFI_IF_STA};
    memcpy(p.peer_addr, mac, 6);
    return esp_now_add_peer(&p);
}

static void reply(const uint8_t *req, uint8_t op, esp_err_t err)
{
    uint8_t p[ENB_HEADER] = {ENB_VERSION, op};
    memcpy(p + 2, req + 2, 8);
    p[11] = err == ESP_OK ? 0 : 1;
    wifi_second_chan_t second;
    (void)esp_wifi_get_channel(p + 10, &second);
    (void)eh_cp_feat_peer_data_send(ENB_MSG_ID, p, sizeof p);
    if (op == ENB_STATE) {
        const char *version = esp_app_get_description()->version;
        const size_t len = strnlen(version, 23);
        uint8_t info[ENB_HEADER + 23] = {ENB_VERSION, ENB_INFO};
        enb_put16(info + 12, (uint16_t)len);
        memcpy(info + ENB_HEADER, version, len);
        (void)eh_cp_feat_peer_data_send(ENB_MSG_ID, info, ENB_HEADER + len);
    }
}

static void worker(void *arg)
{
    (void)arg;
    packet_t p;
    bool waiting = false;
    for (;;) {
        /* After a timeout, do not attribute a late completion to a later send. */
        esp_now_send_status_t result;
        if (waiting && xQueueReceive(completed, &result, 0) == pdTRUE)
            waiting = false;
        for (unsigned i = 0; i < 8 && xQueueReceive(received, &p, 0) == pdTRUE; ++i)
            (void)eh_cp_feat_peer_data_send(ENB_MSG_ID, p.p, p.n);
        if (xQueueReceive(commands, &p, 0) != pdTRUE &&
            xQueueReceive(bulk, &p, pdMS_TO_TICKS(5)) != pdTRUE)
            continue;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        switch (p.p[1]) {
        case ENB_INIT: {
            uint8_t channel;
            wifi_second_chan_t second;
            if (esp_wifi_get_channel(&channel, &second) != ESP_OK) {
                reply(p.p, ENB_STATE, ESP_ERR_INVALID_STATE);
                break;
            }
        }
            if (!ready) {
                err = esp_now_init();
                if (err == ESP_OK)
                    err = esp_now_register_recv_cb(rx);
                if (err == ESP_OK)
                    err = esp_now_register_send_cb(sent);
                if (err == ESP_OK)
                    err = peer(broadcast);
                ready = err == ESP_OK;
                if (!ready)
                    (void)esp_now_deinit();
            } else
                err = ESP_OK;
            /* A joined station must stay on its AP's channel. Channel zero means keep it. */
            if (ready && p.p[10]) {
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
                    err = esp_wifi_set_channel(p.p[10], WIFI_SECOND_CHAN_NONE);
                else if (ap.primary != p.p[10])
                    err = ESP_ERR_INVALID_STATE;
            }
            /* A rejected retune does not disable the working radio. Report its
             * actual channel so the host can retry during a disconnected window. */
            reply(p.p, ENB_STATE, ready ? ESP_OK : err);
            break;
        case ENB_ADD:
            known_set(p.p + 4, true);
            if (ready)
                err = peer(p.p + 4);
            reply(p.p, ENB_DONE, err);
            break;
        case ENB_DEL:
            known_set(p.p + 4, false);
            if (ready)
                err = esp_now_del_peer(p.p + 4);
            reply(p.p, ENB_DONE, err);
            break;
        case ENB_SEND:
            if (ready && !waiting) {
                err = peer(p.p + 4);
                if (err == ESP_OK)
                    err = esp_now_send(p.p + 4, p.p + ENB_HEADER, enb_u16(p.p + 12));
                if (err == ESP_OK) {
                    waiting = xQueueReceive(completed, &result, pdMS_TO_TICKS(200)) != pdTRUE;
                    if (waiting || result != ESP_NOW_SEND_SUCCESS)
                        err = ESP_FAIL;
                    if (waiting) {
                        /* Drain the driver before reusing a completion slot. INIT from
                         * the host restores the broadcast peer and callbacks. */
                        (void)esp_now_deinit();
                        ready = false;
                        waiting = false;
                        xQueueReset(completed);
                    }
                }
            }
            reply(p.p, ENB_DONE, err);
            break;
        default: break;
        }
    }
}

void espnow_bridge_start(void)
{
    (void)esp_event_loop_create_default();
    commands = xQueueCreate(8, sizeof(packet_t));
    bulk = xQueueCreate(8, sizeof(packet_t));
    received = xQueueCreate(16, sizeof(packet_t));
    completed = xQueueCreate(1, sizeof(esp_now_send_status_t));
    ESP_ERROR_CHECK(commands && bulk && received && completed ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(eh_cp_feat_peer_data_init());
    ESP_ERROR_CHECK(eh_cp_feat_peer_data_register_callback(ENB_MSG_ID, request, NULL));
    ESP_ERROR_CHECK(xTaskCreate(worker, "espnow_bridge", 6144, NULL, 5, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}
#endif
