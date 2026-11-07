/* CP-003: asynchronous host shim. A blocked Hosted RPC never blocks the link task.
 * LINK-009: only the link manager can authenticate a received frame or move a peer. */
#include "plink_internal.h"
#if CONFIG_ALARM_LINK
#include "alarm_mem.h"
#include "alarm_proto/espnow_bridge_proto.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "net_wifi.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA_READY
#include "eh_host_feat_peer_data.h"

typedef struct {
    uint64_t at;
    size_t len;
    uint8_t bytes[ENB_MAX];
} radio_packet_t;
static QueueHandle_t tx, bulk, rx;
static atomic_uint channel_request, radio_channel;
static atomic_bool available;
static atomic_uint state_at;
static uint16_t sequence;
static const uint8_t bcast[6] = {255, 255, 255, 255, 255, 255};

/* The peers sealed frames went to: only paired ones get those. The C6 passes their frames on
 * ahead of everyone else's (espnow_bridge.c, SEC-012), so it is told about each (ENB_ADD) when it
 * is new and again every 30 s, which covers a co-processor that restarted. A stranger that was
 * only answered (an unsealed HELLO reply) is never named. */
#define NAMED_MAX 16
static portMUX_TYPE named_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t named[NAMED_MAX][6];
static size_t named_n;
static bool named_new;

static void name_peer(const uint8_t *mac)
{
    portENTER_CRITICAL(&named_lock);
    bool have = false;
    for (size_t i = 0; i < named_n && !have; i++)
        have = memcmp(named[i], mac, 6) == 0;
    if (!have) {
        if (named_n == NAMED_MAX) {
            memmove(named[0], named[1], (NAMED_MAX - 1) * 6);
            named_n--;
        }
        memcpy(named[named_n++], mac, 6);
        named_new = true;
    }
    portEXIT_CRITICAL(&named_lock);
}

/* The radio task's: every named peer, as ENB_ADD. */
static void send_named(void)
{
    uint8_t list[NAMED_MAX][6];
    portENTER_CRITICAL(&named_lock);
    const size_t n = named_n;
    memcpy(list, named, n * 6);
    named_new = false;
    portEXIT_CRITICAL(&named_lock);
    for (size_t i = 0; i < n; i++) {
        uint8_t add[ENB_HEADER] = {ENB_VERSION, ENB_ADD};
        enb_put16(add + 2, ++sequence);
        memcpy(add + 4, list[i], 6);
        (void)eh_host_peer_data_send(ENB_MSG_ID, add, sizeof add);
    }
}

/* Not on the ESP-Hosted receive task's stack: incoming() runs only there (one context, never
 * re-entered), and xQueueSend copies it before the next call. */
static radio_packet_t rx_packet;

static void incoming(uint32_t id, const uint8_t *p, size_t n, void *ctx)
{
    (void)id;
    (void)ctx;
    if (!enb_valid(p, n))
        return;
    if (p[1] == ENB_INFO) {
        char version[24] = {0};
        memcpy(version, p + ENB_HEADER, n - ENB_HEADER);
        net_wifi_coprocessor_version(version);
    } else if (p[1] == ENB_STATE) {
        atomic_store(&available, p[11] == 0);
        atomic_store(&radio_channel, p[10]);
        atomic_store(&state_at, (uint32_t)link_now());
    } else if (p[1] == ENB_RECV) {
        rx_packet.at = link_now();
        rx_packet.len = n;
        memcpy(rx_packet.bytes, p, n);
        (void)xQueueSend(rx, &rx_packet, 0);
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    if (eh_host_peer_data_register(ENB_MSG_ID, incoming, NULL) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    radio_packet_t p;
    uint64_t init_at = 0, named_at = 0;
    unsigned last_channel = 255;
    for (;;) {
        net_wifi_status_t wifi;
        net_wifi_get_status(&wifi);
        if (wifi.state == NET_WIFI_STARTING || wifi.state == NET_WIFI_NO_COPROCESSOR) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const uint64_t now = link_now();
        const unsigned ch = atomic_load(&channel_request);
        if (now >= init_at || ch != last_channel) {
            uint8_t init[ENB_HEADER] = {ENB_VERSION, ENB_INIT};
            init[10] = (uint8_t)ch;
            (void)eh_host_peer_data_send(ENB_MSG_ID, init, sizeof init);
            init_at = link_now() + 1000u;
            last_channel = ch;
        }
        if (named_new || now >= named_at) {
            send_named();
            named_at = link_now() + 30000u;
        }
        if ((xQueueReceive(tx, &p, 0) == pdTRUE ||
             xQueueReceive(bulk, &p, pdMS_TO_TICKS(10)) == pdTRUE) &&
            link_now() - p.at <= 200u) {
            (void)eh_host_peer_data_send(ENB_MSG_ID, p.bytes, p.len);
        }
    }
}

esp_err_t link_radio_start(void)
{
    static StaticQueue_t tx_control, bulk_control, rx_control;
    uint8_t *tx_storage = alarm_mem_slow_calloc(16, sizeof(radio_packet_t));
    uint8_t *rx_storage = alarm_mem_slow_calloc(16, sizeof(radio_packet_t));
    if (!tx_storage || !rx_storage) {
        free(tx_storage);
        free(rx_storage);
        return ESP_ERR_NO_MEM;
    }
    tx = xQueueCreateStatic(8, sizeof(radio_packet_t), tx_storage, &tx_control);
    bulk = xQueueCreateStatic(8, sizeof(radio_packet_t), tx_storage + 8 * sizeof(radio_packet_t),
                              &bulk_control);
    rx = xQueueCreateStatic(16, sizeof(radio_packet_t), rx_storage, &rx_control);
    if (!tx || !bulk || !rx)
        return ESP_ERR_NO_MEM;
    return xTaskCreate(radio_task, "plink_radio", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK
                                                                                 : ESP_ERR_NO_MEM;
}

bool link_radio_ready(void)
{
    return atomic_load(&available) &&
           (uint32_t)((uint32_t)link_now() - atomic_load(&state_at)) < 3000u;
}

unsigned link_radio_channel(void)
{
    return atomic_load(&radio_channel);
}
void link_radio_tune(unsigned channel)
{
    atomic_store(&channel_request, channel);
}

bool link_radio_send(const uint8_t *mac, const uint8_t *frame, size_t len)
{
    if (!tx || !link_radio_ready() || link_test_drops(PLINK_TP_ESPNOW) || !frame || !len ||
        len > PLINK_FRAME_ESPNOW)
        return false;
    radio_packet_t p = {.at = link_now(), .len = ENB_HEADER + len};
    p.bytes[0] = ENB_VERSION;
    p.bytes[1] = ENB_SEND;
    const uint8_t type = len >= PLINK_HDR_LEN ? frame[3] : 0;
    const bool low = type == PLINK_T_FW_PAGE || type == PLINK_T_DEV_LIST_PAGE ||
                     type == PLINK_T_DEV_STATE || type == PLINK_T_PREFS || type == PLINK_T_CFG_PAGE;
    p.bytes[11] = low ? 0 : 1;
    enb_put16(p.bytes + 2, ++sequence);
    memcpy(p.bytes + 4, mac ? mac : bcast, 6);
    if (mac != NULL && len > 4u && (frame[4] & PLINK_FLAG_SEALED) != 0u &&
        memcmp(mac, bcast, 6) != 0) {
        name_peer(mac);
    }
    enb_put16(p.bytes + 12, (uint16_t)len);
    memcpy(p.bytes + ENB_HEADER, frame, len);
    return xQueueSend(low ? bulk : tx, &p, 0) == pdTRUE;
}

bool link_radio_receive(plink_addr_t *from, uint8_t *frame, size_t *len)
{
    radio_packet_t p;
    if (!rx || xQueueReceive(rx, &p, 0) != pdTRUE)
        return false;
    *from = (plink_addr_t){.tp = PLINK_TP_ESPNOW, .len = 6};
    memcpy(from->a, p.bytes + 4, 6);
    *len = enb_u16(p.bytes + 12);
    if (*len > PLINK_FRAME_MAX) {
        *len = 0;
        return true;
    }
    memcpy(frame, p.bytes + ENB_HEADER, *len);
    return true;
}
#else
esp_err_t link_radio_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
bool link_radio_ready(void)
{
    return false;
}
unsigned link_radio_channel(void)
{
    return 0;
}
void link_radio_tune(unsigned channel)
{
    (void)channel;
}
bool link_radio_send(const uint8_t *mac, const uint8_t *frame, size_t len)
{
    (void)mac;
    (void)frame;
    (void)len;
    return false;
}
bool link_radio_receive(plink_addr_t *from, uint8_t *frame, size_t *len)
{
    (void)from;
    (void)frame;
    (void)len;
    return false;
}
#endif

size_t link_mtu(void *ctx, uint8_t transport)
{
    (void)ctx;
    return transport == PLINK_TP_ESPNOW ? PLINK_FRAME_ESPNOW : PLINK_FRAME_MAX;
}
#endif
