/*
 * net_wifi.c - Wi-Fi station, SNTP and time zone. On the ESP32-P4 the esp_wifi API is
 * provided by esp_wifi_remote and runs on the C6 over ESP-Hosted (SDIO).
 */
#include "net_wifi.h"
#include "alarm_mem.h"

#include "esp_attr.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm_service.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED_HOST
#include "esp_hosted.h"
#endif

static const char *TAG = "net";
static const char *NS = "net";

#define DEFAULT_TZ      "UTC0"
#define RETRY_MIN_MS    1000u
#define RETRY_MAX_MS    60000u
#define COPROC_RETRY_MS 30000u

static SemaphoreHandle_t s_lock;
static net_wifi_status_t s_status;
static esp_netif_t *s_netif;
static TaskHandle_t s_task;
static bool s_wifi_ready;
static atomic_uint s_interactive_until_ms;
static atomic_uint s_latency_holds; /* net_wifi_latency_owner_t bits held */
static bool s_low_latency_radio; /* network task owns the power-save transition */
static wifi_ps_type_t s_saved_power_save;
static bool s_have_creds;
static bool s_connect_failed;   /* esp_wifi_connect() itself failed: the net task retries */
static int64_t s_down_since_us; /* 0 = not down */
/* The newest connection attempt began then (0: none running). One that neither connects nor fails
 * in CONNECT_STALL_US is started again: seen with the setup Wi-Fi beside the station (2026-10-21),
 * an attempt that never ended left the panel "connecting" for good. */
static int64_t s_connect_since_us;
#define CONNECT_STALL_US 15000000
static bool s_net_trouble;
static bool s_clock_trouble;
static atomic_uint s_time_syncs;
static volatile bool s_required;
static atomic_uint s_required_mask; /* net_wifi_requester_t bits */
static bool s_sntp_started;
static atomic_bool s_sntp_off; /* net_wifi_set_time_server(false): a link keypad's master's time */
static uint32_t s_retry_ms = RETRY_MIN_MS;
static char s_ntp[64]; /* lwIP keeps a pointer to the server name */
static net_wifi_ap_t s_scan[NET_WIFI_SCAN_MAX] ALARM_SLOW_BSS; /* task-only: slow memory */
static size_t s_scan_count;
static bool s_scan_done = true;
static bool s_scanning;
static int64_t s_radio_until_us; /* net task only */
static net_ip_config_t s_ip; /* address settings; only the net task applies them */
static SemaphoreHandle_t s_config_lock;
typedef struct {
    uint32_t version;
    char ssid[33], password[65], token[33];
    net_ip_config_t ip;
} network_trial_t;
static network_trial_t s_trial;
static bool s_trial_active;
static int64_t s_trial_deadline;
static esp_err_t store_credentials(const char *ssid, const char *password);
static esp_err_t store_ip(const net_ip_config_t *in);
static esp_err_t trial_restore(void);
static esp_err_t trial_clear(void);
static void trial_tick(void);

/* Task notification bits */
#define NOTIFY_RECONNECT 0x01u
#define NOTIFY_CONFIG    0x02u
#define NOTIFY_SCAN      0x04u
#define NOTIFY_SCAN_DONE 0x08u
#define NOTIFY_NTP       0x10u
#define NOTIFY_IPCFG     0x20u
#define NOTIFY_ADDR      0x40u
#define NOTIFY_LATENCY   0x80u
#define NOTIFY_RADIO     0x100u
#define NOTIFY_AP        0x200u

/* The setup Wi-Fi (net_wifi_setup_ap): wanted under s_lock, applied by the net task alone. */
static char s_ap_ssid[33], s_ap_pass[65];
static esp_netif_t *s_ap_netif;
static bool s_ap_on;          /* net task: the access point runs */
static atomic_uint s_ap_devices;

void net_wifi_radio_window(void)
{
    if (s_task)
        xTaskNotify(s_task, NOTIFY_RADIO, eSetBits);
}

static void set_state(net_wifi_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = st;
    if (st != NET_WIFI_CONNECTED) {
        s_status.ip[0] = '\0';
        s_status.mask[0] = '\0';
        s_status.gateway[0] = '\0';
        s_status.dns1[0] = '\0';
        s_status.dns2[0] = '\0';
        s_status.rssi = 0;
    }
    xSemaphoreGive(s_lock);
}

static bool load_str(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = len;
    const esp_err_t err = nvs_get_str(h, key, out, &n);
    nvs_close(h);
    return err == ESP_OK;
}

static esp_err_t save_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = (value != NULL) ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void apply_timezone(void)
{
    char tz[64] = DEFAULT_TZ;
    load_str("tz", tz, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();
}

static void time_synced(struct timeval *tv)
{
    (void)tv;
    atomic_fetch_add(&s_time_syncs, 1u);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool first = !s_status.time_synced;
    s_status.time_synced = true;
    xSemaphoreGive(s_lock);
    if (first) {
        ESP_LOGI(TAG, "time synchronized");
    }
    if (s_clock_trouble) {
        s_clock_trouble = false;
        alarm_service_system_trouble(ALARM_TRBL_CLOCK_NOT_SET, false);
    }
}

static void start_sntp(void)
{
    if (s_sntp_started || atomic_load(&s_sntp_off)) {
        return;
    }
    strlcpy(s_ntp, NET_WIFI_DEFAULT_NTP, sizeof(s_ntp));
    load_str("ntp", s_ntp, sizeof(s_ntp));
    if (s_ntp[0] == '\0') {
        strlcpy(s_ntp, NET_WIFI_DEFAULT_NTP, sizeof(s_ntp));
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp);
    cfg.sync_cb = time_synced;
    cfg.wait_for_sync = false;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
    }
}

static esp_err_t apply_credentials(void)
{
    wifi_config_t wc = {0};
    char ssid[33] = "";
    char pass[65] = "";
    s_have_creds = load_str("ssid", ssid, sizeof(ssid)) && ssid[0] != '\0';
    load_str("pass", pass, sizeof(pass));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.ssid, s_have_creds ? ssid : "", sizeof(s_status.ssid));
    xSemaphoreGive(s_lock);
    if (!s_have_creds) {
        set_state(NET_WIFI_UNCONFIGURED);
        return ESP_OK;
    }
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    memcpy(wc.sta.password, pass, strlen(pass));
    wc.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    return esp_wifi_set_config(WIFI_IF_STA, &wc);
}

/* ---- addresses: from the router, or fixed ------------------------------------------------ */

static bool parse_ip(const char *s, esp_ip4_addr_t *out)
{
    return s != NULL && s[0] != '\0' && esp_netif_str_to_ip4(s, out) == ESP_OK && out->addr != 0u;
}

/* A fixed address needs all three, and the router has to be on the panel's own network -
 * otherwise the panel would have no way off it. */
static bool ip_config_ok(const net_ip_config_t *c)
{
    esp_ip4_addr_t scratch;
    if ((c->dns1[0] != '\0' && !parse_ip(c->dns1, &scratch)) ||
        (c->dns2[0] != '\0' && !parse_ip(c->dns2, &scratch))) {
        return false;
    }
    if (!c->use_static) {
        return true;
    }
    esp_ip4_addr_t ip;
    esp_ip4_addr_t mask;
    esp_ip4_addr_t gw;
    if (!parse_ip(c->ip, &ip) || !parse_ip(c->mask, &mask) || !parse_ip(c->gateway, &gw)) {
        return false;
    }
    return (ip.addr & mask.addr) == (gw.addr & mask.addr) && ip.addr != gw.addr;
}

/* Reads the stored settings into s_ip (the net task's copy). A fixed address that can't be
 * used is turned off here and in NVS, so a typo can never leave the panel off the network
 * for good (docs/features/09 NET-011). */
static void load_ip_config(void)
{
    memset(&s_ip, 0, sizeof(s_ip));
    uint8_t mode = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "ipmode", &mode);
        nvs_close(h);
    }
    s_ip.use_static = mode != 0u;
    load_str("ipaddr", s_ip.ip, sizeof(s_ip.ip));
    load_str("ipmask", s_ip.mask, sizeof(s_ip.mask));
    load_str("ipgw", s_ip.gateway, sizeof(s_ip.gateway));
    load_str("ipdns1", s_ip.dns1, sizeof(s_ip.dns1));
    load_str("ipdns2", s_ip.dns2, sizeof(s_ip.dns2));
    if (s_ip.use_static && !ip_config_ok(&s_ip)) {
        ESP_LOGW(TAG, "stored fixed address unusable: asking the router instead");
        s_ip.use_static = false;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "ipmode", 0);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

static void set_dns(esp_netif_dns_type_t which, const char *addr)
{
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    if (!parse_ip(addr, &dns.ip.u_addr.ip4)) {
        return;
    }
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_netif, which, &dns);
}

/* The name servers set here win over the router's. With a fixed address and none set, the
 * router is used: it nearly always forwards lookups, so a fixed address never silently loses
 * them. DHCP overwrites these when a lease arrives, so this runs again after every address. */
static void apply_dns(void)
{
    if (s_netif == NULL) {
        return;
    }
    const char *first = s_ip.dns1[0] != '\0' ? s_ip.dns1 : (s_ip.use_static ? s_ip.gateway : NULL);
    if (first != NULL) {
        set_dns(ESP_NETIF_DNS_MAIN, first);
    }
    if (s_ip.dns2[0] != '\0') {
        set_dns(ESP_NETIF_DNS_BACKUP, s_ip.dns2);
    }
}

/* What the interface actually has right now, whoever set it. */
static void refresh_addr_info(void)
{
    if (s_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) {
        return;
    }
    esp_netif_dns_info_t d1;
    esp_netif_dns_info_t d2;
    memset(&d1, 0, sizeof(d1));
    memset(&d2, 0, sizeof(d2));
    esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &d1);
    esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &d2);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.static_ip = s_ip.use_static;
    /* A fixed address is set on the interface before it joins: don't report one until the
     * panel is really on the network (set_state clears these the same way). */
    if (s_status.state == NET_WIFI_CONNECTED && info.ip.addr != 0u) {
        esp_ip4addr_ntoa(&info.ip, s_status.ip, sizeof(s_status.ip));
        esp_ip4addr_ntoa(&info.netmask, s_status.mask, sizeof(s_status.mask));
        esp_ip4addr_ntoa(&info.gw, s_status.gateway, sizeof(s_status.gateway));
        s_status.dns1[0] = '\0';
        s_status.dns2[0] = '\0';
        if (d1.ip.u_addr.ip4.addr != 0u) {
            esp_ip4addr_ntoa(&d1.ip.u_addr.ip4, s_status.dns1, sizeof(s_status.dns1));
        }
        if (d2.ip.u_addr.ip4.addr != 0u) {
            esp_ip4addr_ntoa(&d2.ip.u_addr.ip4, s_status.dns2, sizeof(s_status.dns2));
        }
    }
    xSemaphoreGive(s_lock);
}

/* Only the net task calls this: esp_netif is not asked the same question from two sides. */
static void apply_ip_config(void)
{
    if (s_netif == NULL) {
        return;
    }
    if (s_ip.use_static) {
        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof(info));
        parse_ip(s_ip.ip, &info.ip);
        parse_ip(s_ip.mask, &info.netmask);
        parse_ip(s_ip.gateway, &info.gw);
        esp_netif_dhcpc_stop(s_netif);
        const esp_err_t err = esp_netif_set_ip_info(s_netif, &info);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fixed address refused (%s): asking the router", esp_err_to_name(err));
            s_ip.use_static = false;
        } else {
            ESP_LOGI(TAG, "fixed address %s/%s via %s", s_ip.ip, s_ip.mask, s_ip.gateway);
        }
    }
    if (!s_ip.use_static) {
        esp_netif_ip_info_t none;
        memset(&none, 0, sizeof(none));
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_set_ip_info(s_netif, &none);
        const esp_err_t err = esp_netif_dhcpc_start(s_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "esp_netif_dhcpc_start: %s", esp_err_to_name(err));
        }
    }
    apply_dns();
    refresh_addr_info();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        const int reason = d ? d->reason : -1;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %u ms", reason, (unsigned)s_retry_ms);
        if (s_down_since_us == 0) {
            s_down_since_us = esp_timer_get_time();
        }
        set_state(s_have_creds ? NET_WIFI_DISCONNECTED : NET_WIFI_UNCONFIGURED);
        net_wifi_fail_t fail = NET_WIFI_FAIL_OTHER;
        switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_CONNECTION_FAIL: fail = NET_WIFI_FAIL_PASSWORD; break;
        case WIFI_REASON_NO_AP_FOUND: fail = NET_WIFI_FAIL_NOT_FOUND; break;
        case WIFI_REASON_ASSOC_LEAVE: fail = NET_WIFI_FAIL_NONE; break; /* we disconnected */
        default: break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.fail = fail;
        xSemaphoreGive(s_lock);
        xTaskNotify(s_task, NOTIFY_RECONNECT, eSetBits);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "associated");
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        atomic_fetch_add(&s_ap_devices, 1u);
        ESP_LOGI(TAG, "a device joined the setup Wi-Fi");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        unsigned n = atomic_load(&s_ap_devices);
        while (n > 0u && !atomic_compare_exchange_weak(&s_ap_devices, &n, n - 1u)) {
        }
        ESP_LOGI(TAG, "a device left the setup Wi-Fi");
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        xTaskNotify(s_task, NOTIFY_SCAN_DONE, eSetBits);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *ev = data;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = NET_WIFI_CONNECTED;
    s_status.fail = NET_WIFI_FAIL_NONE;
    esp_ip4addr_ntoa(&ev->ip_info.ip, s_status.ip, sizeof(s_status.ip));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "connected, IP %s", s_status.ip);
    /* A DHCP lease brings the router's name servers, and ours have to win again; only the net
     * task talks to esp_netif, so ask it to. */
    xTaskNotify(s_task, NOTIFY_ADDR, eSetBits);
    s_retry_ms = RETRY_MIN_MS;
    s_down_since_us = 0;
    if (s_net_trouble) {
        s_net_trouble = false;
        alarm_service_system_trouble(ALARM_TRBL_NETWORK, false);
    }
    start_sntp();
}

void net_wifi_note_interaction(void)
{
    atomic_store(&s_interactive_until_ms, (uint32_t)(esp_timer_get_time() / 1000) + 5000);
}

void net_wifi_hold_low_latency(net_wifi_latency_owner_t owner, bool on)
{
    const unsigned before = on ? atomic_fetch_or(&s_latency_holds, (unsigned)owner)
                               : atomic_fetch_and(&s_latency_holds, ~(unsigned)owner);
    const unsigned after = on ? (before | (unsigned)owner) : (before & ~(unsigned)owner);
    if ((before != 0u) != (after != 0u) && s_task != NULL) {
        xTaskNotify(s_task, NOTIFY_LATENCY, eSetBits); /* the net task applies it now */
    }
}

/* The holds now: the ones taken, and remote viewing's while its five seconds run. */
static unsigned latency_holds(void)
{
    uint32_t until = atomic_load(&s_interactive_until_ms);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const bool viewing = until && (int32_t)(now - until) < 0;
    if (!viewing && until != 0u) {
        /* Forget the deadline once it has passed: 24.9 days later the signed difference would
         * have gone negative again and switched power saving off for weeks. */
        atomic_compare_exchange_strong(&s_interactive_until_ms, &until, 0u);
    }
    return atomic_load(&s_latency_holds) | (viewing ? (unsigned)NET_WIFI_LATENCY_REMOTE : 0u);
}

/* The net task alone changes the radio's power save: off while any hold stands, and the mode it
 * found restored when the last one goes. */
static void low_latency_tick(void)
{
    const unsigned holds = latency_holds();
    if (holds != 0u && !s_low_latency_radio) {
        wifi_ps_type_t previous;
        if (esp_wifi_get_ps(&previous) == ESP_OK &&
            (previous == WIFI_PS_NONE || esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK)) {
            s_saved_power_save = previous;
            s_low_latency_radio = true;
            ESP_LOGI(TAG, "low-latency Wi-Fi for %s%s (previous power save %d)",
                     (holds & NET_WIFI_LATENCY_REMOTE) != 0u ? "remote viewing " : "",
                     (holds & NET_WIFI_LATENCY_LINK) != 0u ? "the panel link" : "", previous);
        }
    } else if (holds == 0u && s_low_latency_radio) {
        if (s_saved_power_save == WIFI_PS_NONE || esp_wifi_set_ps(s_saved_power_save) == ESP_OK) {
            s_low_latency_radio = false;
            ESP_LOGI(TAG, "low-latency Wi-Fi ended: restored power save %d", s_saved_power_save);
        }
    }
}

void net_wifi_set_time_server(bool on)
{
    if (atomic_exchange(&s_sntp_off, !on) != !on && s_task != NULL) {
        xTaskNotify(s_task, NOTIFY_NTP, eSetBits); /* stopped, or started when connected */
    }
}
static bool wifi_bringup(void)
{
    s_netif = esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi co-processor not answering: %s", esp_err_to_name(err));
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM); /* credentials are kept on this side */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);
    load_ip_config();
    apply_ip_config(); /* a fixed address has to be in place before the interface comes up */
    apply_credentials();
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return false;
    }
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_status.mac, sizeof(s_status.mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                 mac[1], mac[2], mac[3], mac[4], mac[5]);
        xSemaphoreGive(s_lock);
    }
    return true;
}

/* ---- the setup Wi-Fi (net task) ----------------------------------------------------------- */

static void apply_ap(void)
{
    char ssid[33], pass[65];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(ssid, s_ap_ssid, sizeof(ssid));
    memcpy(pass, s_ap_pass, sizeof(pass));
    xSemaphoreGive(s_lock);
    if (ssid[0] == '\0') {
        if (s_ap_on && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
            s_ap_on = false;
            atomic_store(&s_ap_devices, 0u);
            ESP_LOGI(TAG, "setup Wi-Fi closed");
        }
        mbedtls_platform_zeroize(pass, sizeof(pass));
        return;
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif != NULL) {
            /* The phone's name server is the panel (its setup answers every name, as a router's
             * setup does): DHCP offers it. */
            esp_netif_dns_info_t dns = {0};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(NET_WIFI_SETUP_AP_IP);
#if CONFIG_LWIP_DHCPS /* without it net_wifi_setup_ap never opens the setup Wi-Fi */
            uint8_t offer = 1; /* OFFER_DNS */
            (void)esp_netif_dhcps_stop(s_ap_netif);
            (void)esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
            (void)esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                         &offer, sizeof(offer));
            (void)esp_netif_dhcps_start(s_ap_netif);
#endif
        }
    }
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, ssid, strlen(ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(ap.ap.password, pass, strlen(pass));
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.capable = true;
    esp_err_t e = s_ap_netif ? esp_wifi_set_mode(WIFI_MODE_APSTA) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    mbedtls_platform_zeroize(&ap, sizeof(ap));
    mbedtls_platform_zeroize(pass, sizeof(pass));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "setup Wi-Fi: %s", esp_err_to_name(e));
        return;
    }
    if (!s_ap_on) {
        ESP_LOGI(TAG, "setup Wi-Fi \"%s\" open at " NET_WIFI_SETUP_AP_IP, ssid);
    }
    s_ap_on = true;
}

esp_err_t net_wifi_setup_ap(const char *ssid, const char *password)
{
    const bool on = ssid != NULL && ssid[0] != '\0';
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
#if !CONFIG_LWIP_DHCPS
    if (on) {
        return ESP_ERR_NOT_SUPPORTED; /* no DHCP server to give the phone its address */
    }
#endif
    if (on && (strlen(ssid) > 32 || password == NULL || strlen(password) < 8 ||
               strlen(password) > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool same = on ? strcmp(s_ap_ssid, ssid) == 0 && strcmp(s_ap_pass, password) == 0
                         : s_ap_ssid[0] == '\0';
    strlcpy(s_ap_ssid, on ? ssid : "", sizeof(s_ap_ssid));
    strlcpy(s_ap_pass, on ? password : "", sizeof(s_ap_pass));
    xSemaphoreGive(s_lock);
    if (!same && s_task != NULL) {
        xTaskNotify(s_task, NOTIFY_AP, eSetBits);
    }
    return ESP_OK;
}

bool net_wifi_setup_ap_on(uint8_t *devices)
{
    if (devices != NULL) {
        const unsigned n = atomic_load(&s_ap_devices);
        *devices = (uint8_t)(n > 255u ? 255u : n);
    }
    return s_ap_on;
}

/* ---- scanning (net task) ------------------------------------------------------------ */

static void connect_now(void);

static void scan_finished(size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan_count = n;
    s_scan_done = true;
    xSemaphoreGive(s_lock);
    s_scanning = false;
}

static void scan_begin(void)
{
    net_wifi_status_t st;
    net_wifi_get_status(&st);
    if (st.state != NET_WIFI_CONNECTED) {
        esp_wifi_disconnect(); /* a connection attempt blocks scanning; resumed afterwards */
    }
    const wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    const esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        scan_finished(0);
        if (st.state != NET_WIFI_CONNECTED) {
            connect_now();
        }
        return;
    }
    s_scanning = true;
}

static void scan_collect(void)
{
    static wifi_ap_record_t recs[NET_WIFI_SCAN_MAX + 8] ALARM_SLOW_BSS; /* net task only */
    uint16_t n = (uint16_t)(sizeof(recs) / sizeof(recs[0]));
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        n = 0;
    }
    net_wifi_ap_t found[NET_WIFI_SCAN_MAX];
    size_t count = 0;
    for (uint16_t i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        size_t j = 0;
        while (j < count && strcmp(found[j].ssid, ssid) != 0) {
            j++;
        }
        if (j == count) {
            if (count == NET_WIFI_SCAN_MAX) {
                continue;
            }
            count++;
            strlcpy(found[j].ssid, ssid, sizeof(found[j].ssid));
            found[j].rssi = INT8_MIN;
        }
        if (recs[i].rssi > found[j].rssi) {
            found[j].rssi = recs[i].rssi;
            found[j].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        }
    }
    for (size_t i = 1; i < count; i++) { /* strongest first */
        for (size_t k = i; k > 0 && found[k].rssi > found[k - 1].rssi; k--) {
            const net_wifi_ap_t t = found[k];
            found[k] = found[k - 1];
            found[k - 1] = t;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_scan, found, count * sizeof(found[0]));
    xSemaphoreGive(s_lock);
    scan_finished(count);
    ESP_LOGI(TAG, "scan found %u networks", (unsigned)count);
}

void net_wifi_coprocessor_version(const char *version)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.coproc_fw, sizeof(s_status.coproc_fw), "%s", version);
    xSemaphoreGive(s_lock);
}

static void read_coprocessor_version(void)
{
#if CONFIG_ESP_HOSTED_HOST
    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_status.coproc_fw[0] == '\0') {
            snprintf(s_status.coproc_fw, sizeof(s_status.coproc_fw),
                     "%" PRIu32 ".%" PRIu32 ".%" PRIu32, ver.major1, ver.minor1, ver.patch1);
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Wi-Fi co-processor firmware %s", s_status.coproc_fw);
    } else {
        ESP_LOGW(TAG, "Wi-Fi co-processor firmware version unknown");
    }
#endif
}

static void connect_now(void)
{
    if (esp_timer_get_time() < s_radio_until_us) {
        s_connect_failed = true;
        return;
    }
    if (!s_have_creds) {
        return;
    }
    if (s_down_since_us == 0) {
        s_down_since_us = esp_timer_get_time(); /* down until the first IP */
    }
    set_state(NET_WIFI_CONNECTING);
    s_connect_since_us = esp_timer_get_time();
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* No DISCONNECTED event follows a call that failed (the co-processor was busy, or
         * Wi-Fi not started yet), so the retry the event would have scheduled is asked for
         * here, or the panel would sit in "connecting" for good. */
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        s_connect_failed = true;
    }
}

static void net_task(void *arg)
{
    (void)arg;
    /* The co-processor may still be booting: keep trying, with a trouble after the first
     * failure (the panel keeps working without network). */
    while (!wifi_bringup()) {
        set_state(NET_WIFI_NO_COPROCESSOR);
        alarm_service_system_trouble(ALARM_TRBL_COPROCESSOR, true);
        esp_wifi_deinit();
        if (s_netif != NULL) {
            esp_netif_destroy_default_wifi(s_netif);
            s_netif = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(COPROC_RETRY_MS));
    }
    alarm_service_system_trouble(ALARM_TRBL_COPROCESSOR, false);
    read_coprocessor_version();
    s_wifi_ready = true;
    apply_ap(); /* asked for before the radio was up */
    connect_now();

    TickType_t retry_at = 0;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));
        trial_tick();
        if ((bits & NOTIFY_RADIO) && !s_scanning && !s_trial_active) {
            s_radio_until_us = esp_timer_get_time() + 5000000;
            (void)esp_wifi_disconnect();
            retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        }
        if (s_connect_failed && retry_at == 0) {
            s_connect_failed = false;
            retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(s_retry_ms);
            s_retry_ms = (s_retry_ms * 2u > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_ms * 2u;
        }
        low_latency_tick();
        if (bits & NOTIFY_AP) {
            apply_ap();
        }
        if (bits & NOTIFY_CONFIG) {
            esp_wifi_disconnect();
            apply_credentials();
            s_retry_ms = RETRY_MIN_MS;
            s_down_since_us = 0;
            connect_now();
        }
        if (bits & NOTIFY_IPCFG) {
            /* Rejoining is what makes a new address take effect: esp_netif hands out the
             * fixed one (or asks the router again) when the interface comes back up. */
            load_ip_config();
            esp_wifi_disconnect();
            apply_ip_config();
            s_retry_ms = RETRY_MIN_MS;
            s_down_since_us = 0; /* a change the owner asked for: give it its own 5 minutes */
            connect_now();
        }
        if (bits & NOTIFY_ADDR) {
            apply_dns();
            refresh_addr_info();
        }
        if (bits & NOTIFY_RECONNECT) {
            retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(s_retry_ms);
            s_retry_ms = (s_retry_ms * 2u > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_ms * 2u;
        }
        if (bits & NOTIFY_SCAN) {
            scan_begin();
        }
        if (bits & NOTIFY_SCAN_DONE) {
            scan_collect();
            net_wifi_status_t st;
            net_wifi_get_status(&st);
            if (st.state != NET_WIFI_CONNECTED) {
                s_retry_ms = RETRY_MIN_MS;
                retry_at = 0;
                connect_now();
            }
        }
        if (bits & NOTIFY_NTP) {
            if (s_sntp_started) {
                esp_netif_sntp_deinit();
                s_sntp_started = false;
            }
            net_wifi_status_t st;
            net_wifi_get_status(&st);
            if (st.state == NET_WIFI_CONNECTED) {
                start_sntp();
            }
        }
        if (retry_at != 0 && !s_scanning && (int32_t)(xTaskGetTickCount() - retry_at) >= 0) {
            retry_at = 0;
            connect_now();
        }

        const int64_t now_us = esp_timer_get_time();
        const bool down_too_long =
            s_down_since_us != 0 &&
            now_us - s_down_since_us > (int64_t)NET_WIFI_TROUBLE_DELAY_S * 1000000;
        const bool want_trouble = s_required && (!s_have_creds || down_too_long);
        if (want_trouble != s_net_trouble) {
            s_net_trouble = want_trouble;
            alarm_service_system_trouble(ALARM_TRBL_NETWORK, want_trouble);
        }
        net_wifi_status_t st;
        net_wifi_get_status(&st);
        if (st.state != NET_WIFI_CONNECTING) {
            s_connect_since_us = 0;
        } else if (s_connect_since_us != 0 && !s_scanning &&
                   now_us - s_connect_since_us > CONNECT_STALL_US) {
            ESP_LOGW(TAG, "the connection attempt to \"%s\" stalled: trying again", st.ssid);
            s_connect_since_us = 0;
            (void)esp_wifi_disconnect();
            retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(1000); /* whether or not an event comes */
        }
        if (st.state == NET_WIFI_CONNECTED) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.rssi = ap.rssi;
                xSemaphoreGive(s_lock);
            }
            refresh_addr_info();
        }
    }
}

esp_err_t net_wifi_start(void)
{
    /* Started once: s_task is set only once the worker exists, so a duplicate start is refused
     * and a start that failed can be tried again (M02). */
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The locks are built privately and published as a pair. Once published, other tasks (the
     * screen's status, the settings calls) may hold or wait on them, so a later failure keeps
     * them and the next attempt uses the same pair rather than making a second one. */
    if (s_lock == NULL) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        SemaphoreHandle_t config_lock = xSemaphoreCreateMutex();
        if (lock == NULL || config_lock == NULL) {
            if (lock != NULL) {
                vSemaphoreDelete(lock);
            }
            if (config_lock != NULL) {
                vSemaphoreDelete(config_lock);
            }
            return ESP_ERR_NO_MEM;
        }
        s_config_lock = config_lock;
        s_lock = lock;
    }
    /* A reboot never accepts an unconfirmed network. The previous profile was
     * committed before any candidate setting was touched. */
    nvs_handle_t trial_nvs;
    if (nvs_open("nettrial", NVS_READONLY, &trial_nvs) == ESP_OK) {
        size_t len = sizeof(s_trial);
        esp_err_t e = nvs_get_blob(trial_nvs, "previous", &s_trial, &len);
        nvs_close(trial_nvs);
        if (e == ESP_OK && (len != sizeof(s_trial) || s_trial.version != 1)) {
            /* A record another firmware wrote: nothing to restore from it, and refusing to
             * start would reboot the panel into the same refusal with no console to clear it.
             * The stored profile below is the one that was committed before the trial. */
            ESP_LOGE(TAG, "unreadable network trial record ignored");
            trial_clear();
            e = ESP_ERR_NVS_NOT_FOUND;
        }
        if (e == ESP_OK) {
            s_trial_active = true;
            if (trial_restore() != ESP_OK) {
                ESP_LOGE(TAG, "the previous network settings could not be restored yet");
            }
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "network trial record: %s", esp_err_to_name(e));
        }
    }
    s_status.state = NET_WIFI_STARTING;
    apply_timezone();
    /* Before 2025: nothing has set the clock since power-up (a clock module that kept the time
     * set it before this, docs/features/35). */
    if (time(NULL) < (time_t)1735689600) {
        s_clock_trouble = true;
        alarm_service_system_trouble(ALARM_TRBL_CLOCK_NOT_SET, true);
    }
    if (xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 5, &s_task, tskNO_AFFINITY) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t net_wifi_time_syncs(void)
{
    return (uint32_t)atomic_load(&s_time_syncs);
}

void net_wifi_clock_set(void)
{
    if (s_clock_trouble) {
        s_clock_trouble = false;
        alarm_service_system_trouble(ALARM_TRBL_CLOCK_NOT_SET, false);
    }
}

void net_wifi_set_required(net_wifi_requester_t who, bool required)
{
    /* One bit per requester: MQTT switching itself off must not silence the trouble that the
     * phone notifications or the Home Assistant link still depend on. */
    if (required) {
        atomic_fetch_or(&s_required_mask, (unsigned)who);
    } else {
        atomic_fetch_and(&s_required_mask, ~(unsigned)who);
    }
    s_required = atomic_load(&s_required_mask) != 0u;
}

void net_wifi_get_status(net_wifi_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

static esp_err_t store_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) > 32 || (password != NULL && strlen(password) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool forget = ssid[0] == '\0';
    esp_err_t err = save_str("ssid", forget ? NULL : ssid);
    if (err == ESP_OK) {
        err = save_str("pass", (forget || password == NULL) ? NULL : password);
    }
    if (err == ESP_OK && s_task != NULL && s_wifi_ready) {
        xTaskNotify(s_task, NOTIFY_CONFIG, eSetBits);
    }
    return err;
}

void net_wifi_get_ip_config(net_ip_config_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t mode = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "ipmode", &mode);
        nvs_close(h);
    }
    out->use_static = mode != 0u;
    load_str("ipaddr", out->ip, sizeof(out->ip));
    load_str("ipmask", out->mask, sizeof(out->mask));
    load_str("ipgw", out->gateway, sizeof(out->gateway));
    load_str("ipdns1", out->dns1, sizeof(out->dns1));
    load_str("ipdns2", out->dns2, sizeof(out->dns2));
}

static esp_err_t store_ip(const net_ip_config_t *in)
{
    if (in == NULL || !ip_config_ok(in)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "ipmode", in->use_static ? 1u : 0u);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    static const struct {
        const char *key;
        size_t offset;
    } FIELDS[] = {
        {"ipaddr", offsetof(net_ip_config_t, ip)},    {"ipmask", offsetof(net_ip_config_t, mask)},
        {"ipgw", offsetof(net_ip_config_t, gateway)}, {"ipdns1", offsetof(net_ip_config_t, dns1)},
        {"ipdns2", offsetof(net_ip_config_t, dns2)},
    };
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]) && err == ESP_OK; i++) {
        const char *value = (const char *)in + FIELDS[i].offset;
        err = save_str(FIELDS[i].key, value[0] != '\0' ? value : NULL);
    }
    if (err == ESP_OK && s_task != NULL && s_wifi_ready) {
        xTaskNotify(s_task, NOTIFY_IPCFG, eSetBits);
    }
    return err;
}

esp_err_t net_wifi_set_timezone(const char *tz)
{
    if (tz == NULL || tz[0] == '\0' || strlen(tz) >= 64) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = save_str("tz", tz);
    if (err == ESP_OK) {
        apply_timezone();
    }
    return err;
}

static esp_err_t trial_clear(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("nettrial", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_erase_key(h, "previous");
        if (e == ESP_ERR_NVS_NOT_FOUND)
            e = ESP_OK;
        if (e == ESP_OK)
            e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        s_trial_active = false;
        mbedtls_platform_zeroize(&s_trial, sizeof(s_trial));
    }
    return e;
}
static esp_err_t trial_restore(void)
{
    esp_err_t e = store_credentials(s_trial.ssid, s_trial.password);
    if (e == ESP_OK)
        e = store_ip(&s_trial.ip);
    if (e == ESP_OK)
        e = trial_clear();
    if (e == ESP_OK)
        ESP_LOGW(TAG, "Unconfirmed remote network change rolled back");
    return e;
}
static void trial_tick(void)
{
    if (!s_config_lock)
        return;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (s_trial_active && esp_timer_get_time() > s_trial_deadline)
        trial_restore();
    xSemaphoreGive(s_config_lock);
}
static esp_err_t trial_begin(const char *token)
{
    if (s_trial_active || !token || strlen(token) != 32 || strspn(token, "0123456789abcdef") != 32)
        return ESP_ERR_INVALID_STATE;
    memset(&s_trial, 0, sizeof(s_trial));
    s_trial.version = 1;
    load_str("ssid", s_trial.ssid, sizeof(s_trial.ssid));
    load_str("pass", s_trial.password, sizeof(s_trial.password));
    net_wifi_get_ip_config(&s_trial.ip);
    strcpy(s_trial.token, token);
    nvs_handle_t h;
    esp_err_t e = nvs_open("nettrial", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "previous", &s_trial, sizeof(s_trial));
        if (e == ESP_OK)
            e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        s_trial_active = true;
        s_trial_deadline = esp_timer_get_time() + 120000000;
    } else
        mbedtls_platform_zeroize(&s_trial, sizeof(s_trial));
    return e;
}
esp_err_t net_wifi_set_credentials(const char *ssid, const char *password)
{
    if (!s_config_lock)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    esp_err_t e = s_trial_active ? ESP_ERR_INVALID_STATE : store_credentials(ssid, password);
    xSemaphoreGive(s_config_lock);
    return e;
}
void net_wifi_retry_connection(void)
{
    if (!s_config_lock) return;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (!s_trial_active && s_task && s_wifi_ready) xTaskNotify(s_task, NOTIFY_CONFIG, eSetBits);
    xSemaphoreGive(s_config_lock);
}

bool net_wifi_enrollment_read(char ssid[33], char password[65])
{
    memset(ssid, 0, 33);
    memset(password, 0, 65);
    if (!s_config_lock) return false;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    nvs_handle_t h;
    bool ok = false;
    if (!s_trial_active && nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sn = 33, pn = 65;
        esp_err_t se = nvs_get_str(h, "ssid", ssid, &sn);
        esp_err_t pe = nvs_get_str(h, "pass", password, &pn);
        ok = se == ESP_OK && ssid[0] && (pe == ESP_OK || pe == ESP_ERR_NVS_NOT_FOUND);
        nvs_close(h);
    }
    xSemaphoreGive(s_config_lock);
    if (!ok) { memset(ssid, 0, 33); mbedtls_platform_zeroize(password, 65); }
    return ok;
}
esp_err_t net_wifi_enrollment_install(const char *ssid, const char *password)
{
    if (!s_config_lock || !ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 64)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t e = s_trial_active ? ESP_ERR_INVALID_STATE : nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        char current[33] = {0};
        size_t len = sizeof current;
        e = nvs_get_str(h, "ssid", current, &len);
        if (e == ESP_ERR_NVS_NOT_FOUND || (e == ESP_OK && !current[0])) {
            /* Password first: an interrupted save never installs a new SSID with an old password. */
            e = nvs_set_str(h, "pass", password);
            if (e == ESP_OK) e = nvs_set_str(h, "ssid", ssid);
            if (e == ESP_OK) e = nvs_commit(h);
        } else if (e == ESP_OK) e = ESP_ERR_INVALID_STATE;
        nvs_close(h);
    }
    if (e == ESP_OK && s_task && s_wifi_ready) xTaskNotify(s_task, NOTIFY_CONFIG, eSetBits);
    xSemaphoreGive(s_config_lock);
    return e;
}
esp_err_t net_wifi_set_ip_config(const net_ip_config_t *in)
{
    if (!s_config_lock)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    esp_err_t e = s_trial_active ? ESP_ERR_INVALID_STATE : store_ip(in);
    xSemaphoreGive(s_config_lock);
    return e;
}
esp_err_t net_wifi_trial_credentials(const char *ssid, const char *password, const char *token)
{
    if (!s_config_lock || !ssid || !ssid[0] || strlen(ssid) > 32 || !password ||
        strlen(password) > 64)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    esp_err_t e = trial_begin(token);
    if (e == ESP_OK) {
        e = store_credentials(ssid, password);
        if (e != ESP_OK)
            trial_restore();
    }
    xSemaphoreGive(s_config_lock);
    return e;
}
esp_err_t net_wifi_trial_ip(const net_ip_config_t *in, const char *token)
{
    if (!s_config_lock || !in || !ip_config_ok(in))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    esp_err_t e = trial_begin(token);
    if (e == ESP_OK) {
        e = store_ip(in);
        if (e != ESP_OK)
            trial_restore();
    }
    xSemaphoreGive(s_config_lock);
    return e;
}
bool net_wifi_trial_confirm(const char *token)
{
    if (!s_config_lock || !token || strlen(token) != 32)
        return false;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    net_wifi_status_t status;
    net_wifi_get_status(&status);
    bool ok = s_trial_active && esp_timer_get_time() < s_trial_deadline &&
              esp_timer_get_time() >= s_trial_deadline - 117000000 &&
              status.state == NET_WIFI_CONNECTED && strcmp(token, s_trial.token) == 0;
    if (ok)
        ok = trial_clear() == ESP_OK;
    xSemaphoreGive(s_config_lock);
    return ok;
}
esp_err_t net_wifi_trial_cancel(const char *token)
{
    if (!s_config_lock || !token || strlen(token) != 32)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    esp_err_t e = s_trial_active && strcmp(token, s_trial.token) == 0 ? trial_restore()
                                                                      : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_config_lock);
    return e;
}
bool net_wifi_trial_pending(void)
{
    if (!s_config_lock)
        return false;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    bool pending = s_trial_active;
    xSemaphoreGive(s_config_lock);
    return pending;
}

void net_wifi_get_timezone(char *out, size_t len)
{
    strlcpy(out, DEFAULT_TZ, len);
    load_str("tz", out, len);
}

esp_err_t net_wifi_set_ntp_server(const char *server)
{
    if (server == NULL || strlen(server) >= sizeof(s_ntp)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = save_str("ntp", server[0] != '\0' ? server : NULL);
    if (err == ESP_OK && s_task != NULL) {
        xTaskNotify(s_task, NOTIFY_NTP, eSetBits);
    }
    return err;
}

void net_wifi_get_ntp_server(char *out, size_t len)
{
    strlcpy(out, NET_WIFI_DEFAULT_NTP, len);
    if (!load_str("ntp", out, len) || out[0] == '\0') {
        strlcpy(out, NET_WIFI_DEFAULT_NTP, len);
    }
}

esp_err_t net_wifi_scan_start(void)
{
    if (s_task == NULL || !s_wifi_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan_done = false;
    s_scan_count = 0;
    xSemaphoreGive(s_lock);
    xTaskNotify(s_task, NOTIFY_SCAN, eSetBits);
    return ESP_OK;
}

size_t net_wifi_scan_results(net_wifi_ap_t *out, size_t max, bool *done)
{
    if (s_lock == NULL) {
        *done = true;
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = (max < s_scan_count) ? max : s_scan_count;
    memcpy(out, s_scan, n * sizeof(out[0]));
    *done = s_scan_done;
    xSemaphoreGive(s_lock);
    return n;
}
