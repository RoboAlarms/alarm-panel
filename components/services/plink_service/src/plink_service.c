/*
 * plink_service.c - the panel link's frame of work on a panel (docs/features/11, BL-048): the role,
 * the link task and its UDP socket, NVS "link", mDNS, the request hand-over and the copies the link
 * task publishes. The master's side is plink_master_svc.c, a keypad's plink_keypad_svc.c, the UI's
 * entries plink_backend.c and the console plink_console.c.
 *
 * The link task has an internal-RAM stack (it writes NVS at pairing and removal). It waits in
 * select() on the socket for LINK_POLL_MS at most, or less when the manager's next timer is due,
 * hands every datagram of at most 1200 bytes to the manager with its source address, then looks at
 * the hand-overs and runs the timers. It never waits for the alarm task (the master's worker does)
 * and never takes board_ui_lock.
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "alarm_mem.h"
#include "alarm_proto/proto.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "net_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "panel_log.h"

static const char *TAG = "plink";

#define LINK_STACK    8192u /* internal RAM: NVS writes, PSA's X25519, the record's encoding */
#define LINK_PRIORITY 5
#define RX_BURST      16u /* datagrams taken per wake before the timers run */
#define EPOCH_2025_MS 1735689600000LL

link_t g_link = {.sock = -1};
link_pub_t g_pub;

static SemaphoreHandle_t s_pub_lock;

/* The request hand-over (link_request): the callers queue on s_req_mutex, the link task takes the
 * request from s_req under s_req_mux and answers through s_req_done. */
static SemaphoreHandle_t s_req_mutex;
static SemaphoreHandle_t s_req_done;
static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
static link_req_t s_req;
static uint32_t s_req_gen;
static bool s_req_pending;
static uint8_t s_req_result;
static uint32_t s_req_result_gen;

static uint8_t s_rx[PLINK_FRAME_MAX + 1u] ALARM_SLOW_BSS; /* the link task's */

/* ---- the role -------------------------------------------------------------------------------- */

plink_service_role_t plink_service_role(void)
{
    static bool read;
    static plink_service_role_t role = PLINK_SERVICE_MASTER;
    if (!read) {
        read = true;
        uint8_t v = PLINK_SERVICE_MASTER;
        nvs_handle_t h;
        if (nvs_open(LINK_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, LINK_KEY_ROLE, &v) != ESP_OK) {
                v = PLINK_SERVICE_MASTER; /* nothing stored: every panel before the link */
            }
            nvs_close(h);
        }
        if (v == PLINK_SERVICE_KEYPAD) {
            role = PLINK_SERVICE_KEYPAD;
        } else if (v != PLINK_SERVICE_MASTER) {
            /* a role this firmware doesn't know (a newer one stored it): protect the house */
            ESP_LOGE(TAG, "stored role %u not known to this firmware: running as the alarm panel",
                     (unsigned)v);
        }
    }
    return role;
}

/* ---- small helpers --------------------------------------------------------------------------- */

void link_pub_lock(void)
{
    if (s_pub_lock != NULL) {
        xSemaphoreTake(s_pub_lock, portMAX_DELAY);
    }
}

void link_pub_unlock(void)
{
    if (s_pub_lock != NULL) {
        xSemaphoreGive(s_pub_lock);
    }
}

uint64_t link_now(void)
{
    return (uint64_t)alarm_service_now();
}

int64_t link_epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return ms < EPOCH_2025_MS ? 0 : ms;
}

plink_addr_t link_addr_of(uint32_t ip_net_order)
{
    uint8_t ip[4];
    memcpy(ip, &ip_net_order, sizeof ip);
    return plink_addr_udp(ip, PLINK_UDP_PORT);
}

void link_addr_ip(const plink_addr_t *a, char out[24])
{
    if (a != NULL && a->tp == PLINK_TP_UDP && a->len >= 4u) {
        snprintf(out, 24, "%u.%u.%u.%u", a->a[0], a->a[1], a->a[2], a->a[3]);
    } else {
        plink_addr_text(a, out);
    }
}

bool link_wifi_up(void)
{
    net_wifi_status_t st;
    net_wifi_get_status(&st);
    return st.state == NET_WIFI_CONNECTED;
}

bool link_subnet_broadcast(plink_addr_t *out)
{
    uint32_t b = 0xFFFFFFFFu; /* 255.255.255.255 until the panel has an address */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0u) {
        b = ip.ip.addr | ~ip.netmask.addr; /* both in network order: bitwise is the same */
    }
    *out = link_addr_of(b);
    return true;
}

bool link_send(const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    if (g_link.sock < 0 || to == NULL || to->tp != PLINK_TP_UDP || to->len < 6u ||
        len > PLINK_FRAME_MAX) {
        return false;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    memcpy(&sa.sin_addr.s_addr, to->a, 4u); /* both in network order already */
    memcpy(&sa.sin_port, to->a + 4, 2u);
    const ssize_t n = sendto(g_link.sock, frame, len, 0, (const struct sockaddr *)&sa, sizeof sa);
    return n == (ssize_t)len;
}

/* mDNS is shared (ha_link, dev_remote): initialised once by whoever comes first, the same host
 * name, each adding and removing only its own service, never freed (NET-006). */
bool link_mdns_init(void)
{
    static bool up;
    if (up) {
        return true;
    }
    if (!link_wifi_up() || mdns_init() != ESP_OK) {
        return false; /* the network stack isn't ready yet: asked again later */
    }
    char host[32];
    snprintf(host, sizeof host, "roboalarms-%s", g_link.panel_id);
    mdns_hostname_set(host);
    up = true;
    return true;
}

/* ---- NVS "link" ------------------------------------------------------------------------------ */

bool link_nvs_read(const char *key, void *buf, size_t cap, size_t *len)
{
    nvs_handle_t h;
    if (nvs_open(LINK_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = cap;
    const esp_err_t err = nvs_get_blob(h, key, buf, &n);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    *len = n;
    return true;
}

esp_err_t link_nvs_write(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(LINK_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = buf != NULL ? nvs_set_blob(h, key, buf, len) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* erasing what isn't there */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t link_nvs_u32(const char *key, uint32_t value, bool erase)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(LINK_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = erase ? nvs_erase_key(h, key) : nvs_set_u32(h, key, value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool link_nvs_get_u32(const char *key, uint32_t *out)
{
    nvs_handle_t h;
    if (nvs_open(LINK_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_get_u32(h, key, out) == ESP_OK;
    nvs_close(h);
    return ok;
}

esp_err_t link_store_role(plink_service_role_t role)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(LINK_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, LINK_KEY_ROLE, (uint8_t)role);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void link_reset_into(plink_service_role_t role)
{
    static const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_CONSOLE};
    ESP_LOGW(TAG, "factory reset into the %s role",
             role == PLINK_SERVICE_KEYPAD ? "keypad" : "master");
    /* As panel_system_factory_reset: the reset is logged and the engine writes nothing more (a
     * keypad has no engine: the call answers at once), then NVS goes, the role with it. */
    (void)alarm_service_factory_reset(&src, 0, false);
    panel_log_flush(2000);
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        err = nvs_flash_init();
    }
    if (err == ESP_OK && role != PLINK_SERVICE_MASTER) {
        err = link_store_role(role); /* nothing stored is the master */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the reset into the role failed (%s): restarting as it is",
                 esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* ---- requests from the UI and the console ---------------------------------------------------- */

uint8_t link_request(const link_req_t *req)
{
    if (__atomic_load_n(&g_link.task, __ATOMIC_ACQUIRE) == NULL || s_req_mutex == NULL) {
        return UI_LINK_REQ_FAILED; /* the link is off (see the log) */
    }
    if (xTaskGetCurrentTaskHandle() == g_link.task) {
        return UI_LINK_REQ_BUSY; /* would wait for itself */
    }
    xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    (void)xSemaphoreTake(s_req_done, 0); /* a late answer to a request given up on */
    portENTER_CRITICAL(&s_req_mux);
    s_req = *req;
    const uint32_t gen = ++s_req_gen;
    s_req_pending = true;
    portEXIT_CRITICAL(&s_req_mux);
    uint8_t r = UI_LINK_REQ_BUSY;
    if (xSemaphoreTake(s_req_done, pdMS_TO_TICKS(LINK_REQ_WAIT_MS)) == pdTRUE) {
        portENTER_CRITICAL(&s_req_mux);
        if (s_req_result_gen == gen) {
            r = s_req_result;
        }
        portEXIT_CRITICAL(&s_req_mux);
    } else {
        portENTER_CRITICAL(&s_req_mux);
        if (s_req_pending && s_req_gen == gen) {
            s_req_pending = false; /* never taken: withdrawn */
        }
        portEXIT_CRITICAL(&s_req_mux);
    }
    xSemaphoreGive(s_req_mutex);
    return r;
}

static bool take_request(link_req_t *out, uint32_t *gen)
{
    bool got = false;
    portENTER_CRITICAL(&s_req_mux);
    if (s_req_pending) {
        *out = s_req;
        *gen = s_req_gen;
        s_req_pending = false;
        got = true;
    }
    portEXIT_CRITICAL(&s_req_mux);
    return got;
}

static void answer_request(uint32_t gen, uint8_t result)
{
    portENTER_CRITICAL(&s_req_mux);
    s_req_result = result;
    s_req_result_gen = gen;
    portEXIT_CRITICAL(&s_req_mux);
    xSemaphoreGive(s_req_done);
}

/* ---- the link task --------------------------------------------------------------------------- */

static bool is_master(void)
{
    return g_link.role == PLINK_SERVICE_MASTER;
}

/* A datagram to its manager. On a master, frames from a master (FROM_MASTER, header byte 4) can
 * only be for the setup's own pairing as a keypad (Option C); the managers check the rest. */
static void route(uint64_t now, const plink_addr_t *from, const uint8_t *frame, size_t len)
{
    if (!is_master()) {
        ksvc_input(now, from, frame, len);
    } else if (len > 4u && (frame[4] & PLINK_FLAG_FROM_MASTER) != 0u) {
        if (ksvc_active()) {
            ksvc_input(now, from, frame, len);
        }
    } else {
        msvc_input(now, from, frame, len);
    }
}

static uint64_t next_due(void)
{
    uint64_t next = is_master() ? msvc_next() : UINT64_MAX;
    if (ksvc_active()) {
        const uint64_t k = ksvc_next();
        next = k < next ? k : next;
    }
    return next;
}

static void link_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t now = link_now();
        const uint64_t next = next_due();
        uint32_t wait_ms = LINK_POLL_MS;
        if (next <= now) {
            wait_ms = 0u;
        } else if (next - now < wait_ms) {
            wait_ms = (uint32_t)(next - now);
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_link.sock, &rd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)(wait_ms * 1000u)};
        const int ready = select(g_link.sock + 1, &rd, NULL, NULL, &tv);
        if (ready > 0 && FD_ISSET(g_link.sock, &rd)) {
            for (unsigned i = 0; i < RX_BURST; i++) {
                struct sockaddr_in sa;
                socklen_t sl = sizeof sa;
                const ssize_t n = recvfrom(g_link.sock, s_rx, sizeof s_rx, MSG_DONTWAIT,
                                           (struct sockaddr *)&sa, &sl);
                if (n < 0) {
                    break; /* drained */
                }
                if (n == 0 || (size_t)n > PLINK_FRAME_MAX || sa.sin_family != AF_INET) {
                    continue; /* LINK-005: never longer than 1200 bytes */
                }
                uint8_t ip[4];
                memcpy(ip, &sa.sin_addr.s_addr, sizeof ip);
                const plink_addr_t from = plink_addr_udp(ip, ntohs(sa.sin_port));
                route(link_now(), &from, s_rx, (size_t)n);
            }
        }
        now = link_now();
        link_req_t req;
        uint32_t gen;
        if (take_request(&req, &gen)) {
            const uint8_t r = is_master() ? msvc_request(now, &req) : ksvc_request(now, &req);
            plink_wipe(&req, sizeof req);
            answer_request(gen, r);
        }
        if (is_master()) {
            msvc_poll(link_now());
        }
        if (ksvc_active()) {
            ksvc_poll(link_now());
        }
    }
}

static esp_err_t open_socket(void)
{
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return ESP_ERR_NO_MEM;
    }
    const int on = 1;
    /* broadcast: a keypad's HELLO and PAIR_REQ when mDNS finds nothing (LINK-021) */
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PLINK_UDP_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (const struct sockaddr *)&sa, sizeof sa) != 0) {
        close(s);
        return ESP_FAIL;
    }
    const int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    g_link.sock = s;
    return ESP_OK;
}

esp_err_t plink_service_start(const char *model)
{
    if (g_link.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    g_link.role = plink_service_role();
    snprintf(g_link.model, sizeof g_link.model, "%s", model != NULL ? model : "RoboAlarms");
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(g_link.fw, sizeof g_link.fw, "%.*s", (int)PLINK_FW_MAX, app->version);
    /* The panel id is the P4's factory MAC (it has no Wi-Fi MAC of its own), as ha_link's. */
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no factory MAC (%s): the link stays off", esp_err_to_name(err));
        return err;
    }
    char id[PROTO_PANEL_ID_LEN];
    proto_panel_id_from_mac(mac, id);
    snprintf(g_link.panel_id, sizeof g_link.panel_id, "%.*s", (int)PLINK_PANEL_ID_LEN, id);

    /* LINK-003: a port that fails its known answers never carries a frame. */
    if (!plink_psa_port(&g_link.crypto) || !plink_crypto_check(&g_link.crypto)) {
        ESP_LOGE(TAG, "the link's cryptography failed its check: the panel link stays off");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_pub_lock == NULL) {
        s_pub_lock = xSemaphoreCreateMutex();
        s_req_mutex = xSemaphoreCreateMutex();
        s_req_done = xSemaphoreCreateBinary();
        if (s_pub_lock == NULL || s_req_mutex == NULL || s_req_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (g_link.sock < 0) {
        err = open_socket();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "UDP port %u: %s", (unsigned)PLINK_UDP_PORT, esp_err_to_name(err));
            return err;
        }
    }
    const uint64_t now = link_now();
    err = is_master() ? msvc_prepare(now) : ksvc_prepare(now, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the %s side couldn't start: %s", is_master() ? "master's" : "keypad's",
                 esp_err_to_name(err));
        return err;
    }
    link_pub_lock();
    g_pub.running = true;
    link_pub_unlock();
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCore(link_task, "plink", LINK_STACK, NULL, LINK_PRIORITY, &task,
                                tskNO_AFFINITY) != pdPASS) {
        link_pub_lock();
        g_pub.running = false;
        link_pub_unlock();
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&g_link.task, task, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "panel link running as %s on UDP %u (panel %s)",
             is_master() ? "the alarm panel" : "a keypad", (unsigned)PLINK_UDP_PORT,
             g_link.panel_id);
    return ESP_OK;
}

#endif /* CONFIG_ALARM_LINK */
