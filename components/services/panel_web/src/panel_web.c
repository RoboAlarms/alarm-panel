/* Panel Manager: embedded assets, HTTPS and bounded authenticated sessions.
 * No desktop bridge is involved. Alarm operations run on the existing alarm task. */
#include "panel_web.h"
#include "alarm_mem.h"
#include "esp_attr.h"
#include "alarm/alarm_strings.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_service.h"
#include "backup_service.h"
#include "dev_cert.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "http_redirect/http_redirect.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lang_packs.h"
#include "web_voice.h"
#include "lwip/sockets.h"
#if CONFIG_ALARM_LINK
#include "plink_service.h"
#endif
#include "lwip/tcp.h"
#include "mbedtls/platform_util.h"
#include "miniz.h" /* the ROM's inflate (esp_rom) */
#include "net_wifi.h"
#include "notify/notify_auto.h"
#include "notify/notify_words.h"
#include "nvs.h"
#include "panel_log.h"
#include "panel_ota.h"
#include "panel_prefs.h"
#include "psa/crypto.h"
#include "ui_port_esp.h"
#include "web_automations.h"
#include "web_config.h"
#include "web_extra.h"
#include "web_login.h"
#include "web_keypads.h"
#include "web_mem.h"
#include "web_rules.h"
#include "web_screen.h"
#include "web_setup.h"
#include "web_zigbee.h"
#include "web_zwave.h"
#include "ui_backend/ui_why_words.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define SESSION_COUNT   4
#define BODY_LIMIT      49152
#define SESSION_IDLE_US (20LL * 60 * 1000000)
#define SESSION_MAX_US  (8LL * 60 * 60 * 1000000)
/* How long a typed password stands for the guarded changes (recent_password()). */
#define RECENT_PASSWORD_US (10LL * 60 * 1000000)
/* URI handler slots of the HTTPS server: the remote keypad's WebSocket and every route() at its
 * start, with room to spare (each slot is a few dozen bytes); tools/tests/test_panel_web_routes.py
 * counts them (docs/bugs/B017). */
#define WEB_URI_HANDLERS 16
static const alarm_source_t SOURCE = {.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_PANEL_MANAGER};
static dev_remote_adapter_t A;
static ui_backend_t B;
static atomic_bool web_on, dev_on;
static atomic_uint generation;
static SemaphoreHandle_t settings_lock;
static httpd_handle_t server;
static httpd_handle_t redirect_server; /* port 80, only while `server` runs (WEB-023) */
/* A board without a screen is set up from a phone over plain HTTP on its setup Wi-Fi (the owner,
 * 2026-10-21: the first setup, phone to board): port 80 then also serves the pages and the setup's
 * API to a phone on the setup Wi-Fi while setup or recovery is open (plain_serve), and both
 * servers' handlers run one at a time under s_serve_lock (the sessions are one table). */
static bool s_setup_board;
static SemaphoreHandle_t s_serve_lock;
static char identity[13], boot[65];
static uint8_t cert[1024] ALARM_SLOW_BSS; /* PEM for mbedTLS to parse: slow memory */
static uint8_t key[256];
static size_t cert_len, key_len;
typedef struct {
    char token[65], csrf[65];
    uint8_t user, proof[32];
    bool grant; /* opened by an enrolled workstation's grant, not a password */
    int64_t created, used;
    int64_t confirmed; /* this person's password last typed here; 0: never (a grant) */
} session_t;
static session_t sessions[SESSION_COUNT] ALARM_SLOW_BSS; /* HTTP worker owns sessions */
static session_t *controller;
static int64_t next_login, next_frame, control_until;
static uint32_t pointer_sequence;
static char staged_operation[33];
static uint8_t staged_elf[32];
static uint8_t staged_user;
static uint8_t grant_hash[32];
static int64_t grant_until;
extern const uint8_t html_start[] asm("_binary_panel_html_gz_start");
extern const uint8_t html_end[] asm("_binary_panel_html_gz_end");
extern const uint8_t css_start[] asm("_binary_panel_css_gz_start");
extern const uint8_t css_end[] asm("_binary_panel_css_gz_end");
extern const uint8_t js_start[] asm("_binary_panel_js_gz_start");
extern const uint8_t js_end[] asm("_binary_panel_js_gz_end");
extern const uint8_t pages_start[] asm("_binary_panel_pages_js_gz_start");
extern const uint8_t pages_end[] asm("_binary_panel_pages_js_gz_end");
extern const uint8_t keypad_start[] asm("_binary_panel_keypad_js_gz_start");
extern const uint8_t keypad_end[] asm("_binary_panel_keypad_js_gz_end");
extern const uint8_t words_start[] asm("_binary_panel_words_js_gz_start");
extern const uint8_t words_end[] asm("_binary_panel_words_js_gz_end");
/* gzipped at build time (gzip_asset.py) and served straight from the signed image (flash),
 * never copied into RAM. */
static const struct {
    const char *uri;
    const uint8_t *start, *end;
    const char *type;
} ASSETS[] = {
    {"/", html_start, html_end, "text/html; charset=utf-8"},
    {"/panel.css", css_start, css_end, "text/css; charset=utf-8"},
    {"/panel.js", js_start, js_end, "text/javascript; charset=utf-8"},
    {"/panel-pages.js", pages_start, pages_end, "text/javascript; charset=utf-8"},
    {"/panel-keypad.js", keypad_start, keypad_end, "text/javascript; charset=utf-8"},
    {"/panel-words.js", words_start, words_end, "text/javascript; charset=utf-8"},
};
static char asset_tag[24]; /* "<16 hex of this image's ELF SHA-256>" (the gzipped files) */
static char asset_tag_plain[sizeof(asset_tag) + 2]; /* "<the same>p" (the files inflated) */

static void hex(const uint8_t *p, size_t n, char *s)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        s[i * 2] = h[p[i] >> 4];
        s[i * 2 + 1] = h[p[i] & 15];
    }
    s[n * 2] = 0;
}
static void random_token(char out[65])
{
    uint8_t x[32];
    esp_fill_random(x, sizeof(x));
    hex(x, sizeof(x), out);
}
static bool digest(const void *p, size_t n, uint8_t out[32])
{
    size_t got = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, p, n, out, 32, &got) == PSA_SUCCESS && got == 32;
}
static bool equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}
static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool number(const cJSON *o, const char *key, uint32_t lo, uint32_t hi, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        floor(v->valuedouble) != v->valuedouble || v->valuedouble < lo || v->valuedouble > hi)
        return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}
/* The battery-backed clock module (docs/features/35), in the words the page uses. Left out when the
 * firmware has none (no clock_info): the page then says nothing about one. false: no memory to
 * attach it (web_attach(), code review M01). */
static bool add_clock(cJSON *o)
{
    if (!B.clock_info)
        return true;
    static const char *const MODULE[] = {"none", "ok", "lost", "failed", "other"};
    static const char *const SOURCE[] = {"none", "module", "internet", "manual"};
    ui_clock_info_t c;
    memset(&c, 0, sizeof(c));
    B.clock_info(B.ctx, &c);
    cJSON *k = cJSON_CreateObject();
    cJSON_AddStringToObject(k, "module", c.module < 5 ? MODULE[c.module] : "none");
    cJSON_AddStringToObject(k, "source", c.source < 4 ? SOURCE[c.source] : "none");
    cJSON_AddBoolToObject(k, "settable", B.clock_set && ui_clock_settable(&c));
    cJSON_AddBoolToObject(k, "lost_at_start", c.lost_at_start);
    if (c.temp_known)
        cJSON_AddNumberToObject(k, "temp_c", c.temp_c4 / 4.0);
    return web_attach(o, "clock", k);
}
/* ---- who may use what ---------------------------------------------------------------------
 * The panel's own table (docs/features/14 "Who can change what"), one entry per API path. The
 * browser only shows or hides pages; every request is decided here, before its handler runs.
 * Anything not listed is the installer's (fail closed). */
typedef enum {
    ACC_ANYONE,    /* anyone signed in - at the panel, what needs no code */
    ACC_USERS,     /* manages users: the installer, a system or partition master */
    ACC_SHARED,    /* Wi-Fi, time, software updates: a system or partition master, the installer */
    ACC_SYSTEM,    /* integrations, SD card, update source: the system master or the installer */
    ACC_INSTALLER, /* installer programming, remote access, developer tools */
} access_t;
static bool allowed(alarm_authority_t a, access_t need)
{
    switch (need) {
    case ACC_ANYONE: return true;
    case ACC_USERS:
    case ACC_SHARED:
        return a == ALARM_AUTH_INSTALLER || a == ALARM_AUTH_MASTER ||
               a == ALARM_AUTH_PARTITION_MASTER;
    case ACC_SYSTEM: return a == ALARM_AUTH_INSTALLER || a == ALARM_AUTH_MASTER;
    default: return a == ALARM_AUTH_INSTALLER;
    }
}
static bool is(const char *path, const char *name)
{
    return !strcmp(path, name);
}
/* The partitions a signed-in user sees: every one for the installer and the system master,
 * their own for anyone else (as a keypad in their partition would show). false: no memory to
 * read the configuration with - answer 503 "Memory busy", never a refusal (BL-113). */
static bool viewer_partitions(uint8_t user, alarm_authority_t authority, uint8_t *mask)
{
    *mask = 0;
    if (authority == ALARM_AUTH_INSTALLER || authority == ALARM_AUTH_MASTER) {
        *mask = 0xFF;
        return true;
    }
    alarm_config_t *c = web_alloc(sizeof(*c));
    if (!c)
        return false;
    if (alarm_service_get_config(c) == ESP_OK && user >= 1 && user <= ALARM_MAX_USERS)
        *mask = c->users[user - 1].partitions;
    mbedtls_platform_zeroize(c, sizeof(*c));
    free(c);
    return true;
}
static access_t get_access(const char *path)
{
    if (is(path, "status") || is(path, "frame") || is(path, "screen") || is(path, "history") ||
        is(path, "activity") || is(path, "time") ||
        is(path, "zone-status") || is(path, "troubles") || is(path, "storage") || is(path, "config/display") ||
        is(path, "languages") || is(path, "voices")) /* voices: Display and sound's Announcements */
        return ACC_ANYONE;
    if (is(path, "people"))
        return ACC_USERS;
    /* walk-test: the walk test page's list (docs/features/40 WALK-001, WALK-025: the list stays
     * with those who may start one; the Overview's count is in status, for everyone). */
    if (is(path, "config/network") || is(path, "network-check") || is(path, "wifi-scan") ||
        is(path, "walk-test"))
        return ACC_SHARED;
    /* automations, automations/devices: the system master's and the installer's, as Settings >
     * Automations (docs/features/41 DESIGN.md section 8); updates: Software updates, theirs too
     * (OTA-022) - the server, its key and the channel in it only for the installer (get()). */
    if (is(path, "updates") || is(path, "config/mqtt") || is(path, "config/ntfy") || is(path, "config/pushover") ||
        is(path, "home_assistant") || is(path, "notification-status") || is(path, "automations") ||
        is(path, "automations/devices"))
        return ACC_SYSTEM;
    return ACC_INSTALLER; /* alarm, sources, binding/N, developer/logs and anything new */
}
static access_t post_access(const char *path, const cJSON *o)
{
    if (is(path, "config/display")) {
        /* The language and the region are the household's, like the time zone; the rest of
         * display and sound, the clock included, anyone's (docs/features/14, /37 I18N-013). */
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "values");
        return cJSON_GetObjectItemCaseSensitive(v, "lang") != NULL ||
                       cJSON_GetObjectItemCaseSensitive(v, "region") != NULL
                   ? ACC_SHARED
                   : ACC_ANYONE;
    }
    if (is(path, "logout") || is(path, "password") || is(path, "control"))
        return ACC_ANYONE;
    /* Arming, disarming, bypass and the chime (WEB-024): anyone signed in may ask; the engine
     * decides by the user's own authority and partitions, as for their code at a keypad. A walk
     * test's start and extend are refused here too, before the engine (it refuses them as well):
     * the installer's, a system or partition master's (WALK-001). Its end is anyone's who may
     * disarm the partition, which the engine decides (WALK-025). */
    if (is(path, "partition")) {
        const char *a = str(o, "action");
        return !strcmp(a, "walk_test_start") || !strcmp(a, "walk_test_extend") ? ACC_SHARED
                                                                                : ACC_ANYONE;
    }
    if (is(path, "people"))
        return ACC_USERS;
    if (is(path, "config/network") || is(path, "wifi") || is(path, "time") ||
        is(path, "network-check") || is(path, "wifi-scan") || is(path, "languages"))
        return ACC_SHARED; /* languages: the same people who may change the language */
    if (is(path, "updates")) {
        /* OTA-022: checking, installing and stopping are the system master's and the
         * installer's; the update server and the channel the installer's alone. */
        const char *a = str(o, "action");
        return !strcmp(a, "server_check") || !strcmp(a, "server") || !strcmp(a, "channel")
                   ? ACC_INSTALLER
                   : ACC_SYSTEM;
    }
    if (is(path, "config/mqtt")) {
        /* Panics over MQTT set off alarms: the installer's, like arming without a code; the
         * rest of the page the household's too (docs/features/14, BL-107). */
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "values");
        return cJSON_GetObjectItemCaseSensitive(v, "allow_panic") != NULL ? ACC_INSTALLER
                                                                          : ACC_SYSTEM;
    }
    if (is(path, "config/ntfy") || is(path, "config/pushover") ||
        is(path, "home_assistant") || is(path, "storage") || is(path, "notification-test") ||
        is(path, "ntfy-topic") || is(path, "certificate-authority") ||
        is(path, "automations")) /* docs/features/41 DESIGN.md section 8; not while locked */
        return ACC_SYSTEM;
    return ACC_INSTALLER;
}
/* While a partition is armed (from its exit time on) or an alarm sounds, nothing changes - as
 * on the touchscreen (alarm_snapshot_locked). The remote keypad, the partition commands, your
 * own password and signing out stay: disarming is how the lock ends, and the engine itself
 * refuses what a state forbids (bypass or the chime while armed). A walk test's start is a change
 * like Settings' (the touchscreen starts one there, locked while armed); its extend and end stay:
 * one already running on a disarmed partition keeps going or ends, as a disarm does (WALK-025). */
static bool locked_exempt(const char *path, const cJSON *o)
{
    if (is(path, "partition")) {
        const char *a = str(o, "action");
        return strcmp(a, "walk_test_start") != 0;
    }
    /* Stopping an update is allowed while armed (OTA-027); checking and installing are not. */
    if (is(path, "updates"))
        return !strcmp(str(o, "action"), "cancel");
    return is(path, "logout") || is(path, "password") || is(path, "control");
}
static bool panel_locked(void)
{
    alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
    if (!snap)
        return true; /* can't tell just now: refuse the change rather than guess */
    alarm_service_get_snapshot(snap);
    const bool locked = alarm_snapshot_locked(snap);
    free(snap);
    return locked;
}
static void security_headers(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(
        r, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob: data:; "
        "connect-src 'self'; frame-ancestors 'none'; form-action 'self'; base-uri 'none'");
}
static void headers(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    security_headers(r);
}
static esp_err_t json(httpd_req_t *r, cJSON *o)
{
    if (!o)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory busy");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory busy");
    headers(r);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return e;
}
/* A builder's document: o, or NULL - json() answers "Memory busy" - when one of its parts could
 * not be attached (ok false: web_attach(), code review M01), never a partial answer. */
static cJSON *built(cJSON *o, bool ok)
{
    if (ok)
        return o;
    cJSON_Delete(o);
    return NULL;
}
/* web_extra's answer when its document could not be built (M01): 503, as json() answers ours. */
static bool memory_busy(const cJSON *o)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(o, "error");
    return cJSON_IsString(e) && !strcmp(e->valuestring, "Memory busy");
}
/* Set by the handlers that take a body, cleared once the body has been read in full. A
 * refusal that leaves a body unread closes the connection: the server would otherwise read
 * and discard whatever was announced, 32 bytes at a time, for as long as the other end cared
 * to keep sending it - the one worker task tied up by anyone on the network. */
static bool s_body_unread;
/* The language the page asked for (X-Panel-Language, docs/features/37 I18N-031): the sentences
 * the panel makes for it (events, zone states, check results, the answers marked "why") come in
 * it. NULL - English - without the header or for a language this image doesn't have. */
static const i18n_catalog_t *request_words(httpd_req_t *r)
{
    char lang[24];
    return httpd_req_get_hdr_value_str(r, "X-Panel-Language", lang, sizeof(lang)) == ESP_OK
               ? lang_packs_catalog(lang)
               : NULL;
}
/* One of the answers marked I18N_NOOP_C("why", ...) (web_login.c, ui_port_esp.c, and those that
 * carry a number: ui_why_words.h), in it: written back into why. */
static const char *said(httpd_req_t *r, char *why, size_t why_len)
{
    char line[160];
    ui_why_line(request_words(r), "why", why, line, sizeof(line));
    i18n_utf8_copy(why, why_len, line);
    return why;
}
static esp_err_t error(httpd_req_t *r, const char *status, const char *message)
{
    httpd_resp_set_status(r, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    if (s_body_unread && r->content_len > 0) {
        httpd_resp_set_hdr(r, "Connection", "close");
        (void)json(r, o);
        s_body_unread = false;
        return ESP_FAIL; /* the reply went; this tells httpd to close, not purge */
    }
    return json(r, o);
}
static esp_err_t okay(httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return json(r, o);
}
static esp_err_t result(httpd_req_t *r, alarm_result_t e)
{
    return e == ALARM_OK ? okay(r) : error(r, "409 Conflict", alarm_result_name(e));
}
/* The part after /api/v1/, without any query string: httpd matched the route on the path
 * alone, so "?x" must not turn one action into another (or into a 404). */
static void api_path(httpd_req_t *r, char *out, size_t cap)
{
    const char *p = r->uri + 8;
    snprintf(out, cap, "%.*s", (int)strcspn(p, "?"), p);
}
static bool plain_http(httpd_req_t *r)
{
    return redirect_server != NULL && r->handle == redirect_server;
}
static bool origin_ok(httpd_req_t *r)
{
    char origin[160], host[128], expected[160], site[24];
    if (httpd_req_get_hdr_value_str(r, "Sec-Fetch-Site", site, sizeof(site)) == ESP_OK &&
        !strcmp(site, "cross-site"))
        return false;
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    if (strpbrk(host, "/\\ \r\n\t@"))
        return false;
    if (httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) != ESP_OK)
        return r->method == HTTP_GET;
    snprintf(expected, sizeof(expected), "%s://%s", plain_http(r) ? "http" : "https", host);
    return !strcmp(origin, expected);
}
/* The request came over the setup Wi-Fi (the panel's own address there), not the home network. */
static bool via_setup_wifi(httpd_req_t *r)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(r), (struct sockaddr *)&local, &len) != 0)
        return false;
    char text[48] = "";
    if (local.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&local)->sin_addr, text, sizeof(text));
#if LWIP_IPV6
    else if (local.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&local)->sin6_addr, text, sizeof(text));
#endif
    const char *v4 = strrchr(text, ':'); /* "::ffff:192.168.4.1" */
    return !strcmp(v4 ? v4 + 1 : text, NET_WIFI_SETUP_AP_IP);
}
static bool unique_members(const cJSON *o, unsigned *remaining)
{
    if (!*remaining)
        return false;
    --*remaining;
    unsigned members = 0;
    for (const cJSON *p = o->child; p; p = p->next) {
        if (++members > 256 || !unique_members(p, remaining))
            return false;
        if (cJSON_IsObject(o))
            for (const cJSON *q = o->child; q != p; q = q->next)
                if (!strcmp(p->string, q->string))
                    return false;
    }
    return true;
}
static cJSON *body(httpd_req_t *r)
{
    char type[48];
    if (r->content_len <= 0 || r->content_len > BODY_LIMIT ||
        httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json"))
        return NULL;
    char *buf = web_alloc(r->content_len + 1);
    if (!buf)
        return NULL;
    size_t pos = 0;
    int64_t until = esp_timer_get_time() + 10000000;
    while (pos < r->content_len && esp_timer_get_time() < until) {
        int n = httpd_req_recv(r, buf + pos, r->content_len - pos);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue; /* a pause on the line, not its end: the deadline above bounds it */
        if (n <= 0)
            break;
        pos += (size_t)n;
    }
    if (pos == r->content_len)
        s_body_unread = false;
    buf[pos] = 0;
    const char *end = NULL;
    bool nul_escape = false;
    for (size_t i = 0; i + 5 < pos; i++)
        if (buf[i] == '\\') {
            size_t run = 1;
            while (i + run < pos && buf[i + run] == '\\')
                run++;
            if ((run & 1) && i + run + 4 < pos && !strncmp(buf + i + run, "u0000", 5))
                nul_escape = true;
            i += run - 1;
        }
    /* A raw NUL would end a string early in C while cJSON kept what followed. */
    cJSON *o = pos == r->content_len && !nul_escape && memchr(buf, 0, pos) == NULL
                   ? cJSON_ParseWithLengthOpts(buf, pos + 1, &end, true)
                   : NULL;
    unsigned remaining = 4096;
    bool valid = cJSON_IsObject(o) && end == buf + pos && unique_members(o, &remaining);
    mbedtls_platform_zeroize(buf, r->content_len + 1);
    free(buf);
    if (!valid) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}
static void erase_json(cJSON *o)
{
    if (!o)
        return;
    for (cJSON *p = o->child; p; p = p->next) {
        if (p->valuestring)
            mbedtls_platform_zeroize(p->valuestring, strlen(p->valuestring));
        if (p->child)
            erase_json(p);
    } /* deletion occurs once at root */
}
static void release_control(void)
{
    if (controller && A.control)
        A.control(false);
    controller = NULL;
    control_until = 0;
}
/* verified: a session being opened (or carried on) by a password - built only on the very sign-in
 * that password was checked against (web_login_fingerprint_verified()), never on one turned off
 * or replaced meanwhile, and never on no sign-in at all (code review S01). NULL: an open
 * session's check, or a grant's. */
static bool user_proof(uint8_t user, uint8_t proof[32], alarm_authority_t *authority,
                       const uint8_t *verified)
{
    if (user < 1 || user > ALARM_MAX_USERS)
        return false;
    alarm_config_t *c = web_alloc(sizeof(*c));
    if (!c)
        return false;
    bool ok = alarm_service_get_config(c) == ESP_OK;
    if (ok) {
        /* The user record (code, authority, name) and the browser sign-in: a new code, another
         * role, a new password or a sign-in turned off each end the sessions it opened. */
        struct {
            alarm_user_t user;
            uint8_t login[32];
        } basis;
        basis.user = c->users[user - 1];
        if (verified)
            ok = web_login_fingerprint_verified(c, user, verified, basis.login);
        else
            (void)web_login_fingerprint(c, user, basis.login);
        *authority = basis.user.authority;
        /* Whoever may have a sign-in keeps a session; a code turned arm-only, guest or duress
         * (or deleted) ends it. */
        ok = ok && web_login_eligible(c, user) && digest(&basis, sizeof(basis), proof);
        mbedtls_platform_zeroize(&basis, sizeof(basis));
    }
    mbedtls_platform_zeroize(c, sizeof(*c));
    free(c);
    return ok;
}
static session_t *session(httpd_req_t *r, alarm_authority_t *authority)
{
    if (!atomic_load(&web_on) || !origin_ok(r))
        return NULL;
    /* By name, whatever else the browser keeps for this address: a Cookie header longer than
     * a fixed buffer used to fail every request after a successful sign-in. */
    char token[65];
    size_t token_len = sizeof(token);
    if (httpd_req_get_cookie_val(r, "panel_session", token, &token_len) != ESP_OK ||
        strlen(token) != 64)
        return NULL;
    int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < SESSION_COUNT; i++) {
        session_t *s = &sessions[i];
        if (!s->token[0] || !equal(s->token, token, 64))
            continue;
        uint8_t proof[32];
        const bool have_proof = user_proof(s->user, proof, authority, NULL);
        if (now - s->used > SESSION_IDLE_US || now - s->created > SESSION_MAX_US ||
            (have_proof && !equal(proof, s->proof, 32))) {
            if (controller == s)
                release_control();
            mbedtls_platform_zeroize(s, sizeof(*s));
            return NULL;
        }
        if (!have_proof)
            return NULL; /* couldn't be computed just now (memory, crypto): not a mismatch */
        if (r->method != HTTP_GET) {
            char csrf[65];
            if (httpd_req_get_hdr_value_str(r, "X-Panel-CSRF", csrf, sizeof(csrf)) != ESP_OK ||
                strlen(csrf) != 64 || !equal(csrf, s->csrf, 64))
                return NULL;
        }
        s->used = now;
        return s;
    }
    return NULL;
}
/* ---- the remote keypad's WebSocket (web_screen.c) ---- */
const void *panel_web_ws_session(httpd_req_t *r, uint8_t *user, char *who, size_t who_cap,
                                 char token[65])
{
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    if (!s || (!s->grant && web_login_temporary(s->user)))
        return NULL;
    *user = s->user;
    memcpy(token, s->token, 65);
    alarm_config_t *c = web_alloc(sizeof(*c));
    if (c && alarm_service_get_config(c) == ESP_OK && c->users[s->user - 1].name[0])
        i18n_utf8_copy(who, who_cap, c->users[s->user - 1].name); /* whole characters */
    else
        snprintf(who, who_cap, "%s", s->user == ALARM_USER_INSTALLER ? "Installer" : "A user");
    if (c) {
        mbedtls_platform_zeroize(c, sizeof(*c));
        free(c);
    }
    return s;
}
bool panel_web_ws_alive(const void *session_ptr, const char token[65], const char *csrf)
{
    static int64_t checked; /* the whole proof at most once a second: acks come faster */
    session_t *s = (session_t *)session_ptr;
    if (!atomic_load(&web_on) || s < sessions || s >= sessions + SESSION_COUNT || !s->token[0] ||
        !equal(s->token, token, 64))
        return false;
    const int64_t now = esp_timer_get_time();
    if (now - s->used > SESSION_IDLE_US || now - s->created > SESSION_MAX_US)
        return false;
    if (csrf && (strlen(csrf) != 64 || !equal(csrf, s->csrf, 64)))
        return false;
    if (csrf || now - checked > 1000000) {
        uint8_t proof[32];
        alarm_authority_t a;
        if (!user_proof(s->user, proof, &a, NULL) || !equal(proof, s->proof, 32) ||
            (!s->grant && web_login_temporary(s->user)))
            return false;
        checked = now;
    }
    s->used = now;
    return true;
}
/* A free or expired slot, else the one used longest ago: someone who just proved a password
 * must not be turned away for 20 minutes by tabs left open elsewhere (four sessions). */
static session_t *free_session_slot(void)
{
    int64_t now = esp_timer_get_time();
    session_t *oldest = &sessions[0];
    for (unsigned i = 0; i < SESSION_COUNT; i++) {
        if (!sessions[i].token[0] || now - sessions[i].used > SESSION_IDLE_US)
            return &sessions[i];
        if (sessions[i].used < oldest->used)
            oldest = &sessions[i];
    }
    return oldest;
}
/* verified: the sign-in a password was checked against (web_login_check()); NULL for a grant. */
static session_t *new_session(uint8_t user, const uint8_t *verified)
{
    session_t *s = free_session_slot();
    int64_t now = esp_timer_get_time();
    if (controller == s)
        release_control();
    memset(s, 0, sizeof(*s));
    alarm_authority_t authority;
    if (!user_proof(user, s->proof, &authority, verified))
        return NULL;
    random_token(s->token);
    random_token(s->csrf);
    s->user = user;
    s->created = s->used = now;
    return s;
}
bool panel_web_issue_grant(char token[65])
{
    if (!settings_lock || !atomic_load(&web_on) || !atomic_load(&dev_on))
        return false;
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    random_token(token);
    bool ok = digest(token, 64, grant_hash);
    grant_until = ok ? esp_timer_get_time() + 60000000 : 0;
    xSemaphoreGive(settings_lock);
    return ok;
}
static bool consume_grant(const char *token)
{
    if (!atomic_load(&dev_on) || strlen(token) != 64)
        return false;
    uint8_t hash[32];
    if (!digest(token, 64, hash))
        return false;
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    bool ok = esp_timer_get_time() < grant_until && equal(hash, grant_hash, 32);
    if (ok) {
        grant_until = 0;
        mbedtls_platform_zeroize(grant_hash, sizeof(grant_hash));
    }
    xSemaphoreGive(settings_lock);
    return ok;
}
/* Wrong passwords: five are free, then every sign-in waits a minute, doubling with each
 * further miss up to 15 minutes; an hour without one forgets them. A wrong password typed for
 * a guarded change (recent_password()) counts here too: it is the same guess. */
static web_gate_t s_login_gate;
static esp_err_t login(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    if (!atomic_load(&web_on) || !origin_ok(r))
        return error(r, "403 Forbidden", "Same-origin HTTPS required");
    int64_t now = esp_timer_get_time();
    if (now < next_login)
        return error(r, "429 Too Many Requests", "Wait before trying again");
    next_login = now + 2000000;
    cJSON *o = body(r);
    if (!o)
        return error(r, "400 Bad Request", "Expected JSON");
    uint8_t user = 0;
    uint8_t verified[32] = {0};
    bool ok;
    const bool by_grant = str(o, "grant")[0] != 0;
    if (by_grant) {
        ok = consume_grant(str(o, "grant"));
        user = ALARM_USER_MASTER;
    } else if (strlen(str(o, "password")) < WEB_PASSWORD_MIN) {
        /* Every browser password is at least WEB_PASSWORD_MIN characters (web_login.c), so a
         * shorter one can never match: it is almost always a keypad code (B010). Say so, and
         * leave the wrong-sign-in count alone - a guess that cannot succeed tells a guesser
         * nothing the length rule doesn't, and the owner is not locked out by habit. */
        erase_json(o);
        cJSON_Delete(o);
        return error(r, "400 Bad Request", "password_too_short");
    } else if (!web_gate_open(&s_login_gate)) {
        erase_json(o);
        cJSON_Delete(o);
        return error(r, "429 Too Many Requests", "Too many wrong sign-ins; wait before trying again");
    } else {
        /* A user name and a password set on the panel's screen (web_login.h). Keypad codes
         * no longer sign in: a code seen over a shoulder must not open the panel remotely. */
        ok = web_login_check(str(o, "username"), str(o, "password"), &user, verified);
        if (!ok) {
            web_gate_failed(&s_login_gate);
            ESP_LOGW("panel_web", "wrong browser sign-in");
        }
    }
    erase_json(o);
    cJSON_Delete(o);
    if (!ok)
        return error(r, "401 Unauthorized", "invalid_login");
    session_t *s = new_session(user, by_grant ? NULL : verified);
    mbedtls_platform_zeroize(verified, sizeof(verified));
    /* No longer someone who may sign in, the sign-in turned off or replaced since the password
     * was checked, or the proof could not be made just now. */
    if (!s)
        return error(r, "401 Unauthorized", "invalid_login");
    s->grant = by_grant;
    if (!by_grant)
        s->confirmed = esp_timer_get_time(); /* signing in counts as typing the password */
    ESP_LOGI("panel_web", "user %u signed in to the Panel Manager", (unsigned)user);
    char cookie[160];
    /* Secure only over HTTPS: the setup Wi-Fi's plain HTTP (plain_serve) keeps it too. */
    snprintf(cookie, sizeof(cookie), "panel_session=%s; Path=/;%s HttpOnly; SameSite=Strict",
             s->token, plain_http(r) ? "" : " Secure;");
    httpd_resp_set_hdr(r, "Set-Cookie", cookie);
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "csrf", s->csrf);
    cJSON_AddNumberToObject(o, "user", user);
    cJSON_AddBoolToObject(o, "must_change", !by_grant && web_login_temporary(user));
    return json(r, o);
}
/* Signed in with a temporary password: nothing but changing it (and signing out). */
static bool must_change(const session_t *s)
{
    return !s->grant && web_login_temporary(s->user);
}
/* What is left of this session's recent password, in microseconds (0: the next guarded change
 * asks for it). */
static int64_t password_left_us(const session_t *s)
{
    const int64_t age = esp_timer_get_time() - s->confirmed;
    return s->confirmed && age >= 0 && age < RECENT_PASSWORD_US ? RECENT_PASSWORD_US - age : 0;
}
/* Your password again before the few changes that let a computer in for good, carry every
 * secret away or can't be undone (OWASP ASVS 5.0 V7.5): remote access and developer mode,
 * enrolling a workstation, downloading or restoring a backup, factory reset. A password typed
 * in this session in the last RECENT_PASSWORD_US is enough, the sign-in's included (never a
 * grant's). password: what the request carried (me_password, or a restore's X-Panel-Password);
 * the caller erases it. A wrong one counts with the wrong sign-ins. false: *answer is the
 * refusal, already sent. */
static bool recent_password(httpd_req_t *r, session_t *s, const char *password,
                            esp_err_t *answer)
{
    if (password_left_us(s) > 0)
        return true;
    if (!password || !password[0]) {
        *answer = error(r, "403 Forbidden", "password_required");
        return false;
    }
    /* The same guess as a sign-in: the same spacing between tries, and the same count. */
    const int64_t now = esp_timer_get_time();
    if (now < next_login) {
        *answer = error(r, "429 Too Many Requests", "Wait before trying again");
        return false;
    }
    next_login = now + 2000000;
    if (!web_gate_open(&s_login_gate)) {
        *answer = error(r, "429 Too Many Requests",
                        "Too many wrong sign-ins; wait before trying again");
        return false;
    }
    if (!web_login_verify(s->user, password)) {
        web_gate_failed(&s_login_gate);
        ESP_LOGW("panel_web", "user %u: wrong password for a guarded change", (unsigned)s->user);
        *answer = error(r, "403 Forbidden", "wrong_password");
        return false;
    }
    s->confirmed = esp_timer_get_time();
    return true;
}
/* A q-value of zero ("q=0", "q=0.000"): that coding is refused. p is just past "q=". */
static bool q_zero(const char *p)
{
    if (*p++ != '0')
        return false;
    if (*p == '.')
        p += 1 + strspn(p + 1, "0");
    return *p == 0 || *p == ' ' || *p == '\t' || *p == ';';
}
/* Whether the client takes the gzipped files (RFC 9110 12.5.3): gzip, or "*", listed in
 * Accept-Encoding and not with q=0. Browsers always list it; a client that sends no
 * Accept-Encoding (curl) or asks for identity (Python's http.client) gets the plain file. */
static bool takes_gzip(httpd_req_t *r)
{
    char v[160];
    const esp_err_t e = httpd_req_get_hdr_value_str(r, "Accept-Encoding", v, sizeof(v));
    if (e != ESP_OK && e != ESP_ERR_HTTPD_RESULT_TRUNC)
        return false;
    bool gzip_listed = false, gzip = false, any = false;
    char *save = NULL;
    for (char *item = strtok_r(v, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        item += strspn(item, " \t");
        const size_t name = strcspn(item, " \t;");
        bool refused = false;
        for (const char *p = strchr(item + name, ';'); p; p = strchr(p + 1, ';')) {
            const char *param = p + 1 + strspn(p + 1, " \t");
            if ((param[0] == 'q' || param[0] == 'Q') && param[1] == '=')
                refused = q_zero(param + 2);
        }
        if ((name == 4 && !strncasecmp(item, "gzip", 4)) ||
            (name == 6 && !strncasecmp(item, "x-gzip", 6))) {
            gzip_listed = true;
            gzip = !refused;
        } else if (name == 1 && item[0] == '*') {
            any = !refused;
        }
    }
    return gzip_listed ? gzip : any;
}
/* A file inflated on the way out, for a client that doesn't take gzip: the ROM's tinfl with a
 * 32 KiB window, sent in chunks. gz is gzip_asset.py's layout: a 10-byte header without
 * optional fields, the deflate stream, an 8-byte trailer. */
static esp_err_t send_inflated(httpd_req_t *r, tinfl_decompressor *d, uint8_t *window,
                               const uint8_t *gz, size_t size)
{
    if (size < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8 || gz[3] != 0)
        return ESP_FAIL;
    tinfl_init(d);
    const uint8_t *in = gz + 10;
    size_t left = size - 18, at = 0;
    for (;;) {
        size_t used = left, made = TINFL_LZ_DICT_SIZE - at;
        const tinfl_status s = tinfl_decompress(d, in, &used, window, window + at, &made, 0);
        in += used;
        left -= used;
        if (made && httpd_resp_send_chunk(r, (const char *)window + at, (ssize_t)made) != ESP_OK)
            return ESP_FAIL;
        if (s != TINFL_STATUS_HAS_MORE_OUTPUT)
            return s == TINFL_STATUS_DONE ? httpd_resp_send_chunk(r, NULL, 0) : ESP_FAIL;
        at = (at + made) & (TINFL_LZ_DICT_SIZE - 1);
    }
}
/* A language's Panel Manager words, /i18n/<locale>.json (docs/features/37 I18N-031): made from
 * the catalog already in this image, a piece at a time, so they cost no second copy in flash and
 * no buffer bigger than one piece. English has no file: the page keeps its own words. Anyone may
 * read them, as the sign-in page is worded too. */
static esp_err_t catalog_words(httpd_req_t *r, size_t length)
{
    char locale[24];
    const size_t n = length - 6; /* past "/i18n/" */
    /* The panel's own language, which a browser without a choice of its own opens in. */
    if (n == 5 && !strncmp(r->uri + 6, "panel", 5)) {
        panel_prefs_t prefs;
        panel_prefs_load(&prefs);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "locale", prefs.lang[0] ? prefs.lang : "en-US");
        return json(r, o); /* not kept: the panel's language can change */
    }
    if (n <= 5 || n - 5 >= sizeof(locale) || strncmp(r->uri + length - 5, ".json", 5))
        return error(r, "404 Not Found", "Not found");
    memcpy(locale, r->uri + 6, n - 5);
    locale[n - 5] = 0;
    const i18n_catalog_t *c = lang_packs_catalog(locale);
    if (!c)
        return error(r, "404 Not Found", "Not found");
    /* The words change with the image, like the files, and with a loaded pack: the first 8 hex
     * digits of its SHA-256 follow the tag, so a new pack is fetched without a firmware change
     * (I18N-046). */
    char tag[sizeof(asset_tag_plain) + 12];
    snprintf(tag, sizeof(tag), "%s", asset_tag_plain);
    lang_pack_info_t *list = web_alloc(LANG_PACKS_LIST_MAX * sizeof(*list));
    if (list) {
        const size_t count = lang_packs_list(list, LANG_PACKS_LIST_MAX);
        for (size_t i = 0; i < count; i++)
            if (list[i].loaded && !strcmp(list[i].locale, locale)) {
                const size_t at = strlen(tag);
                snprintf(tag + at, sizeof(tag) - at, "-%02x%02x%02x%02x", list[i].sha256[0],
                         list[i].sha256[1], list[i].sha256[2], list[i].sha256[3]);
                break;
            }
        free(list);
    }
    char seen[sizeof(tag)];
    if (httpd_req_get_hdr_value_str(r, "If-None-Match", seen, sizeof(seen)) == ESP_OK &&
        !strcmp(seen, tag)) {
        httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(r, "ETag", tag);
        security_headers(r);
        httpd_resp_set_status(r, "304 Not Modified");
        return httpd_resp_send(r, NULL, 0);
    }
    enum { PIECE = 4096 };
    char *piece = web_alloc(PIECE);
    if (!piece)
        return error(r, "503 Service Unavailable", "Memory busy");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(r, "ETag", tag);
    security_headers(r);
    httpd_resp_set_type(r, "application/json");
    uint32_t pos = 0;
    size_t made;
    esp_err_t e = ESP_OK;
    while (e == ESP_OK && (made = i18n_web_json(c, &pos, piece, PIECE)) > 0)
        e = httpd_resp_send_chunk(r, piece, (ssize_t)made);
    free(piece);
    return e == ESP_OK ? httpd_resp_send_chunk(r, NULL, 0) : ESP_FAIL;
}
static esp_err_t assets(httpd_req_t *r)
{
    /* The pages are the setup's too: served while setup or recovery is open, the Panel Manager
     * switched off or not (it is off until the installer's access exists). */
    if (!atomic_load(&web_on) && !web_setup_listener())
        return error(r, "503 Service Unavailable", "Web server is disabled");
    size_t length = strcspn(r->uri, "?");
    if (length > 6 && !strncmp(r->uri, "/i18n/", 6))
        return catalog_words(r, length);
    unsigned i = 0;
    while (i < sizeof(ASSETS) / sizeof(ASSETS[0]) &&
           (strlen(ASSETS[i].uri) != length || strncmp(r->uri, ASSETS[i].uri, length)))
        i++;
    if (i == sizeof(ASSETS) / sizeof(ASSETS[0]))
        return error(r, "404 Not Found", "Not found");
    const bool gzip = takes_gzip(r);
    const char *tag = gzip ? asset_tag : asset_tag_plain;
    /* The files change only with the firmware image, so its ELF hash is the validator:
     * a reload costs one small 304 instead of the whole bundle over TLS. */
    char seen[sizeof(asset_tag_plain)];
    if (httpd_req_get_hdr_value_str(r, "If-None-Match", seen, sizeof(seen)) == ESP_OK &&
        !strcmp(seen, tag)) {
        httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(r, "ETag", tag);
        httpd_resp_set_hdr(r, "Vary", "Accept-Encoding");
        security_headers(r);
        httpd_resp_set_status(r, "304 Not Modified");
        return httpd_resp_send(r, NULL, 0);
    }
    tinfl_decompressor *d = NULL;
    uint8_t *window = NULL;
    if (!gzip) {
        d = web_alloc(sizeof(*d));
        window = web_alloc(TINFL_LZ_DICT_SIZE);
        if (!d || !window) {
            free(window);
            free(d);
            return error(r, "503 Service Unavailable", "Memory busy");
        }
    }
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(r, "ETag", tag);
    httpd_resp_set_hdr(r, "Vary", "Accept-Encoding");
    security_headers(r);
    httpd_resp_set_type(r, ASSETS[i].type);
    const size_t size = (size_t)(ASSETS[i].end - ASSETS[i].start);
    if (gzip) {
        httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
        return httpd_resp_send(r, (const char *)ASSETS[i].start, (ssize_t)size);
    }
    const esp_err_t e = send_inflated(r, d, window, ASSETS[i].start, size);
    free(window);
    free(d);
    return e;
}
/* The alarm active in a partition now, the most urgent first (fire, co, burglary, panic, aux),
 * or "" - the alarms the touchscreen's Home shows (ui_app.c compute_mode: MODE_FIRE, MODE_CO,
 * MODE_ALARM, MODE_AUX; the one difference: a burglary kept on screen outranks an auxiliary
 * alarm sounding). The alarm task's disarm check (alarm_service.c command_state_ok) uses the
 * same rule for a disarmed partition. An alarm sounding counts in any state, disarmed included (a smoke
 * detector, a CO detector, a medical button, a panic); a burglary or panic while armed stays an
 * alarm after its siren timed out, while zones are in alarm memory, until the disarm. Burglary or
 * panic: panic only when the police panic is the one audible cause in the memory. A silent alarm
 * is never shown (the engine keeps it off the sounder and, while armed, out of the memory). */
static const char *alarm_kind(const alarm_partition_status_t *ps)
{
    if (ps->sounder == ALARM_SOUND_FIRE)
        return "fire";
    if (ps->sounder == ALARM_SOUND_CO)
        return "co";
    if (ps->sounder == ALARM_SOUND_ALARM || (ps->state == ALARM_STATE_ARMED && ps->alarms > 0)) {
        const uint16_t panic = 1u << ALARM_CAUSE_PANIC_POLICE;
        const uint16_t burglary = (1u << ALARM_CAUSE_BURGLARY) |
                                  (1u << ALARM_CAUSE_SILENT_BURGLARY) |
                                  (1u << ALARM_CAUSE_EXIT_ERROR) | (1u << ALARM_CAUSE_TAMPER) |
                                  (1u << ALARM_CAUSE_24H_AUDIBLE);
        return (ps->memory & panic) && !(ps->memory & burglary) ? "panic" : "burglary";
    }
    return ps->sounder == ALARM_SOUND_AUX ? "aux" : "";
}
/* A user's name as the panel shows it: blank for none, "Installer" for a blank installer slot. */
static const char *user_name(const alarm_config_t *cfg, uint8_t user)
{
    if (user < 1 || user > ALARM_MAX_USERS)
        return "";
    const char *name = cfg->users[user - 1].name;
    return user == ALARM_USER_INSTALLER && !name[0] ? "Installer" : name;
}

/* ---- software updates (docs/features/13 OTA-017..OTA-031; BL-186, BL-188) ----------------
 * Everything panel_ota says is read into the heap (web_alloc): the httpd task's stack is small. */

/* An update runs: from Install (or a developer's file, or the Wi-Fi module's update) to the end of
 * the self-test (OTA-030). */
static bool ota_run_going(const panel_ota_status_t *st)
{
    return st->stage != PANEL_OTA_STAGE_NONE || st->probation;
}
/* The same, read now: 1 or 0; -1: no memory to read it with. */
static int ota_running_now(void)
{
    panel_ota_status_t *st = web_alloc(sizeof(*st));
    if (!st)
        return -1;
    panel_ota_get_status(st);
    const int going = ota_run_going(st) ? 1 : 0;
    free(st);
    return going;
}
/* The installer code is still the factory one (OTA-021): 1 or 0; -1: no memory to tell. */
static int ota_factory_code(void)
{
    alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
    if (!snap)
        return -1;
    alarm_service_get_snapshot(snap);
    const int factory = (snap->default_codes & ALARM_DEFAULT_CODE_INSTALLER) ? 1 : 0;
    free(snap);
    return factory;
}
static const char *ota_stage_name(panel_ota_stage_t st)
{
    switch (st) {
    case PANEL_OTA_STAGE_DOWNLOADING: return "downloading";
    case PANEL_OTA_STAGE_VERIFYING: return "verifying";
    case PANEL_OTA_STAGE_WAITING: return "waiting";
    case PANEL_OTA_STAGE_INSTALLING: return "installing";
    case PANEL_OTA_STAGE_STOPPING: return "stopping";
    case PANEL_OTA_STAGE_ROLLING_BACK: return "rolling_back";
    case PANEL_OTA_STAGE_WIFI_MODULE: return "wifi_module";
    case PANEL_OTA_STAGE_FILE: return "file";
    default: return "";
    }
}
static const char *ota_kind_name(panel_ota_kind_t k)
{
    switch (k) {
    case PANEL_OTA_KIND_RELEASE: return "release";
    case PANEL_OTA_KIND_FILE: return "file";
    case PANEL_OTA_KIND_WIFI_MODULE: return "wifi_module";
    default: return "";
    }
}
static const char *ota_last_name(panel_ota_last_result_t l)
{
    switch (l) {
    case PANEL_OTA_LAST_UPDATED: return "updated";
    case PANEL_OTA_LAST_ROLLED_BACK: return "rolled_back";
    case PANEL_OTA_LAST_STOPPED: return "stopped";
    case PANEL_OTA_LAST_FAILED: return "failed";
    default: return "";
    }
}
static const char *ota_srv_result_name(panel_ota_srv_result_t res)
{
    switch (res) {
    case PANEL_OTA_SRV_OK: return "ok";
    case PANEL_OTA_SRV_UNREACHABLE: return "unreachable";
    case PANEL_OTA_SRV_NOT_A_SERVER: return "not_a_server";
    case PANEL_OTA_SRV_NO_FIRMWARE: return "no_firmware";
    case PANEL_OTA_SRV_BAD_DOMAIN: return "bad_domain";
    default: return "";
    }
}
/* A signing key as the pages show it (OTA-020): its name, the 16-digit fingerprint and all 64
 * hex digits of its SHA-256 (lower case). */
static cJSON *ota_key(const char *name, const uint8_t digest[32])
{
    char fp[PROTO_FWSRV_FINGERPRINT_LEN], hex[65];
    proto_fwsrv_fingerprint(digest, fp);
    for (size_t i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", (unsigned)digest[i]);
    cJSON *k = cJSON_CreateObject();
    cJSON_AddStringToObject(k, "name", name);
    cJSON_AddStringToObject(k, "fingerprint", fp);
    cJSON_AddStringToObject(k, "sha256", hex);
    return k;
}
/* Exactly 64 hex digits (either case) into 32 bytes; false for anything else. */
static bool ota_digest(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64)
        return false;
    for (size_t i = 0; i < 32; i++) {
        unsigned v = 0;
        for (size_t j = 0; j < 2; j++) {
            const unsigned char c = (unsigned char)hex[2 * i + j];
            if (!isxdigit(c))
                return false;
            v = v * 16u + (unsigned)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
    return true;
}
/* GET updates (OTA-022: the system master's and the installer's, get_access): the version, the
 * offer, the run (OTA-026..OTA-029) and how the last one ended; the update server, its key, the
 * channel and the last server check for the installer only (OTA-017..OTA-021). */
static esp_err_t updates_get(httpd_req_t *r, alarm_authority_t authority)
{
    struct ota_view {
        panel_ota_status_t st;
        panel_ota_server_t srv;
        panel_ota_srv_check_t chk;
        char line[160];
    } *v = web_alloc(sizeof(*v));
    if (!v)
        return error(r, "503 Service Unavailable", "Memory busy");
    const bool installer = authority == ALARM_AUTH_INSTALLER;
    panel_ota_get_status(&v->st);
    const panel_ota_status_t *st = &v->st;
    /* Who the last run was for or stopped by, by name ("" for the system). */
    const char *by = "";
    alarm_config_t *cfg = NULL;
    if (st->last.result != PANEL_OTA_LAST_NONE && st->last.by_user != 0) {
        cfg = web_alloc(sizeof(*cfg));
        if (!cfg || alarm_service_get_config(cfg) != ESP_OK) {
            free(cfg);
            free(v);
            return error(r, "503 Service Unavailable", "Memory busy");
        }
        by = user_name(cfg, st->last.by_user);
    }
    int factory = 0;
    if (installer) {
        factory = ota_factory_code();
        if (factory < 0) {
            if (cfg)
                mbedtls_platform_zeroize(cfg, sizeof(*cfg));
            free(cfg);
            free(v);
            return error(r, "503 Service Unavailable", "Memory busy");
        }
        panel_ota_get_server(&v->srv);
        panel_ota_server_check_get(&v->chk);
    }
    const i18n_catalog_t *words = request_words(r);
    cJSON *o = cJSON_CreateObject();
    bool ok = true;
    cJSON_AddStringToObject(o, "running", st->running);
    cJSON_AddStringToObject(o, "latest", st->latest);
    cJSON_AddBoolToObject(o, "available", st->available);
    cJSON_AddBoolToObject(o, "has_source", st->server[0] != '\0' || st->source[0] != '\0');
    cJSON_AddBoolToObject(o, "withdrawn", st->withdrawn);
    cJSON_AddBoolToObject(o, "critical", st->critical);
    cJSON_AddStringToObject(o, "installed_channel", st->installed_channel);
    /* Why it failed, or what holds the run back ("The system is armed"), in the page's words. */
    const char *why = st->phase == PANEL_OTA_FAILED ? st->error
                      : st->reason[0]               ? st->reason
                                                    : st->waiting_for;
    ui_why_line(words, "update", why, v->line, sizeof(v->line));
    cJSON_AddStringToObject(o, "reason", v->line);
    cJSON_AddNumberToObject(o, "phase", st->phase); /* the same numbers as ui_fw_phase_t */
    cJSON_AddNumberToObject(o, "percent", st->percent);
    cJSON_AddBoolToObject(o, "confirming", st->probation);
    /* The run (OTA-026..OTA-028). */
    cJSON_AddStringToObject(o, "stage", ota_stage_name(st->stage));
    cJSON_AddStringToObject(o, "kind", ota_kind_name(st->kind));
    cJSON_AddStringToObject(o, "target", st->target);
    cJSON_AddStringToObject(o, "previous", st->previous);
    cJSON_AddNumberToObject(o, "bytes", st->bytes);
    cJSON_AddNumberToObject(o, "size", st->size);
    cJSON_AddStringToObject(o, "failed", st->phase != PANEL_OTA_FAILED ? ""
                                         : st->failed_install         ? "install"
                                                                      : "check");
    /* How the last run ended (OTA-029); it replaces "rolled_back". */
    if (st->last.result != PANEL_OTA_LAST_NONE) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "result", ota_last_name(st->last.result));
        cJSON_AddStringToObject(l, "version", st->last.version);
        cJSON_AddStringToObject(l, "from", st->last.from);
        cJSON_AddNumberToObject(l, "at", st->last.at);
        ui_why_line(words, "update", st->last.reason, v->line, sizeof(v->line));
        cJSON_AddStringToObject(l, "reason", v->line);
        cJSON_AddStringToObject(l, "by", by);
        ok &= web_attach(o, "last", l);
    } else {
        cJSON_AddNullToObject(o, "last");
    }
    if (cfg)
        mbedtls_platform_zeroize(cfg, sizeof(*cfg)); /* it holds the code hashes too */
    free(cfg);
    if (installer) {
        /* The domain only: the full address is never shown (OTA-017). */
        cJSON *srv = cJSON_CreateObject();
        cJSON_AddStringToObject(srv, "domain", v->srv.domain);
        cJSON_AddBoolToObject(srv, "official", v->srv.official);
        if (v->srv.has_key)
            ok &= web_attach(srv, "key", ota_key(v->srv.key_name, v->srv.key_sha256));
        else
            cJSON_AddNullToObject(srv, "key");
        ok &= web_attach(o, "server", srv);
        cJSON_AddStringToObject(o, "channel", st->channel);
        cJSON_AddBoolToObject(o, "factory_code", factory == 1);
        /* The last server check (OTA-018, OTA-020): its key only when it found one. */
        const panel_ota_srv_check_t *c = &v->chk;
        cJSON *chk = cJSON_CreateObject();
        cJSON_AddStringToObject(chk, "state", c->state == PANEL_OTA_SRV_CHECKING ? "checking"
                                              : c->state == PANEL_OTA_SRV_DONE   ? "done"
                                                                                 : "");
        cJSON_AddStringToObject(chk, "domain", c->domain);
        const bool done = c->state == PANEL_OTA_SRV_DONE;
        cJSON_AddStringToObject(chk, "result", done ? ota_srv_result_name(c->result) : "");
        if (done && c->result == PANEL_OTA_SRV_OK)
            ok &= web_attach(chk, "key", ota_key(c->key_name, c->key_sha256));
        else
            cJSON_AddNullToObject(chk, "key");
        cJSON_AddBoolToObject(chk, "trusted", done && c->result == PANEL_OTA_SRV_OK && c->trusted);
        ok &= web_attach(o, "check", chk);
    }
    free(v);
    return json(r, built(o, ok));
}
/* Where a walk test was started (docs/features/40 WALK-024), as a word the page translates:
 * "panel" (this touchscreen), "panel_manager", "keypad" (a remote keypad), "console", "remote"
 * (MQTT or a server), "zone" (a keyfob or keyswitch), else the source's own name. */
static const char *walk_from(const alarm_source_t *src)
{
    if (src->kind == ALARM_SRC_KEYPAD)
        return src->id == ALARM_KEYPAD_LOCAL     ? "panel"
               : src->id == ALARM_KEYPAD_CONSOLE ? "console"
                                                 : "keypad";
    if (src->kind == ALARM_SRC_REMOTE)
        return src->id == ALARM_SRC_ID_PANEL_MANAGER ? "panel_manager" : "remote";
    return alarm_source_name((alarm_source_kind_t)src->kind);
}
/* Whole seconds of a walk test's time left, less the snapshot's age; 0 when none runs. */
static uint32_t walk_left_s(const alarm_partition_status_t *ps, alarm_ms_t age)
{
    if (!ps->walk_test || ps->walk_test_remaining_ms <= age)
        return 0;
    return (uint32_t)((ps->walk_test_remaining_ms - age + 999u) / 1000u);
}
/* One partition as the Overview shows it (WEB-024): its state, how and by whom it was armed, the
 * time left (age: how old the snapshot is), which levels could arm now, and what the touchscreen
 * would say about an alarm. */
static cJSON *partition_status(const alarm_snapshot_t *snap, const alarm_config_t *cfg, unsigned i,
                               alarm_ms_t age)
{
    const alarm_partition_status_t *ps = &snap->part[i];
    const alarm_partition_config_t *pc = &cfg->partitions[i];
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "number", i + 1);
    cJSON_AddStringToObject(p, "name", pc->name);
    cJSON_AddStringToObject(p, "state", alarm_state_name(ps->state));
    cJSON_AddBoolToObject(p, "ready", ps->ready);
    cJSON_AddNumberToObject(p, "faults", ps->faults);
    cJSON_AddStringToObject(p, "level", alarm_level_name(ps->level));
    cJSON_AddBoolToObject(p, "no_entry_delay", (ps->arm_flags & ALARM_ARM_NO_ENTRY_DELAY) != 0);
    cJSON_AddBoolToObject(p, "silent_exit", (ps->arm_flags & ALARM_ARM_SILENT_EXIT) != 0);
    cJSON_AddNumberToObject(p, "armed_by", ps->armed_by);
    cJSON_AddStringToObject(p, "armed_by_name", user_name(cfg, ps->armed_by));
    /* docs/features/41 D30, AUT-009 (additive): who armed it, read before armed_by - an
     * automation acts as armed_by (who saved it), who is then never shown as the one who armed.
     * armed_source null while disarmed or not known; armed_automation the automation's words
     * (English, as MQTT), null when it can't say (none, after a restart, or deleted). */
    if (ps->state != ALARM_STATE_DISARMED && ps->arm_source.kind != (uint8_t)ALARM_SRC_SYSTEM) {
        cJSON_AddStringToObject(p, "armed_source",
                                alarm_source_name((alarm_source_kind_t)ps->arm_source.kind));
    } else {
        cJSON_AddNullToObject(p, "armed_source");
    }
    notify_auto_info_t auto_info;
    const notify_auto_found_t auto_found =
        ps->arm_source.kind == (uint8_t)ALARM_SRC_AUTOMATION
            ? notify_auto_lookup(NULL, cfg, ps->arm_source.id, &auto_info)
            : NOTIFY_AUTO_UNKNOWN;
    if (auto_found == NOTIFY_AUTO_FOUND && (auto_info.sentence[0] != '\0' || auto_info.clause[0] != '\0')) {
        cJSON_AddStringToObject(p, "armed_automation",
                                auto_info.sentence[0] != '\0' ? auto_info.sentence : auto_info.clause);
    } else {
        cJSON_AddNullToObject(p, "armed_automation");
    }
    /* The engine's time left counts only in exit or entry time, from when the snapshot was
     * taken: less what has passed since. */
    const bool timing = ps->state == ALARM_STATE_EXIT_DELAY || ps->state == ALARM_STATE_ENTRY_DELAY;
    const alarm_ms_t left = timing && ps->delay_remaining_ms > age ? ps->delay_remaining_ms - age : 0;
    cJSON_AddNumberToObject(p, "delay_left_ms", (double)left);
    cJSON_AddNumberToObject(p, "exit_delay_s", pc->exit_delay_s);
    cJSON *entry = cJSON_CreateArray();
    bool ok = web_attach(entry, NULL, cJSON_CreateNumber(pc->entry_delay1_s));
    ok &= web_attach(entry, NULL, cJSON_CreateNumber(pc->entry_delay2_s));
    ok &= web_attach(p, "entry_delay_s", entry);
    cJSON *ready = cJSON_CreateArray();
    static const alarm_arm_level_t LEVELS[] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY, ALARM_LEVEL_NIGHT};
    for (size_t l = 0; l < sizeof(LEVELS) / sizeof(LEVELS[0]); l++)
        if (ps->ready_levels & ALARM_LEVEL_BIT(LEVELS[l]))
            ok &= web_attach(ready, NULL, cJSON_CreateString(alarm_level_name(LEVELS[l])));
    ok &= web_attach(p, "ready_levels", ready);
    cJSON_AddBoolToObject(p, "chime", ps->chime);
    const char *kind = alarm_kind(ps);
    /* "memory" is the touchscreen's MODE_MEMORY test (alarm memory, not only a canceled mark, or
     * zones in it), and like it only while disarmed with no alarm active. */
    const bool memory = ps->state == ALARM_STATE_DISARMED && !kind[0] &&
                        ((ps->memory & (uint16_t)~ALARM_MEM_CANCELED) != 0 || ps->alarms > 0);
    cJSON_AddBoolToObject(p, "alarm", kind[0] != '\0');
    cJSON_AddStringToObject(p, "alarm_kind", kind);
    cJSON_AddBoolToObject(p, "siren", ps->bell != ALARM_PAT_OFF);
    cJSON_AddBoolToObject(p, "memory", memory);
    cJSON_AddBoolToObject(p, "canceled", memory && (ps->memory & ALARM_MEM_CANCELED) != 0);
    cJSON_AddNumberToObject(p, "bypassed", ps->bypassed);
    cJSON_AddBoolToObject(p, "trouble_unack", ps->trouble_unack);
    /* A walk test running on it, wherever it was started (docs/features/40 WALK-024): who
     * started it, from where, the count and the time left, for everyone who sees the partition
     * (whoever may disarm it may end it, WALK-025). Zero and blank while none runs. */
    const bool walk = ps->walk_test;
    cJSON_AddBoolToObject(p, "walk_test", walk);
    cJSON_AddNumberToObject(p, "walk_tested", walk ? ps->walk_tested : 0);
    cJSON_AddNumberToObject(p, "walk_total", walk ? ps->walk_total : 0);
    cJSON_AddNumberToObject(p, "walk_remaining_s", walk_left_s(ps, age));
    cJSON_AddNumberToObject(p, "walk_started_by", walk ? ps->walk_started_by : 0);
    cJSON_AddStringToObject(p, "walk_started_by_name", walk ? user_name(cfg, ps->walk_started_by) : "");
    cJSON_AddStringToObject(p, "walk_started_from", walk ? walk_from(&ps->walk_started_src) : "");
    return built(p, ok);
}
/* GET walk-test (docs/features/40 WALK-017, WALK-018): each partition the viewer may use, with
 * its walk test - running or the last one's summary - and each zone the test counts with its
 * mark; the zones that stay live (fire, CO, and 24-hour and panic zones unless the start
 * included them) by name only. ?since=<seq>&trips=<n>: when both are still the sums over these
 * partitions, only {unchanged, seq, trips, partitions: [{number, running, remaining_s}]}, so a
 * page polling every second stays small. */
static const char *const WALK_END[] = {"", "command", "disarm", "time_limit"};
static const char *const WALK_HOW[] = {"fault", "cover", "button", "sensor_test"};
static bool query_number(httpd_req_t *r, const char *key, uint32_t *out)
{
    char q[64], v[12];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK || !v[0] || strlen(v) > 10)
        return false;
    uint64_t n = 0;
    for (const char *c = v; *c; c++) {
        if (*c < '0' || *c > '9')
            return false;
        n = n * 10u + (uint64_t)(*c - '0');
    }
    if (n > UINT32_MAX)
        return false;
    *out = (uint32_t)n;
    return true;
}
static cJSON *walk_partition(const alarm_snapshot_t *snap, const alarm_config_t *cfg, unsigned i,
                             alarm_ms_t now, alarm_ms_t age)
{
    const alarm_partition_status_t *ps = &snap->part[i];
    const bool any = ps->walk_test || ps->walk_end_how != ALARM_WALK_END_NONE;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "number", i + 1);
    cJSON_AddStringToObject(p, "name", cfg->partitions[i].name);
    cJSON_AddBoolToObject(p, "running", ps->walk_test);
    cJSON_AddNumberToObject(p, "remaining_s", walk_left_s(ps, age));
    cJSON_AddNumberToObject(p, "started_by", ps->walk_started_by);
    cJSON_AddStringToObject(p, "started_by_name", user_name(cfg, ps->walk_started_by));
    cJSON_AddStringToObject(p, "started_from", any ? walk_from(&ps->walk_started_src) : "");
    cJSON_AddNumberToObject(p, "started_s_ago", any && now >= ps->walk_started_ms
                                                    ? (double)((now - ps->walk_started_ms) / 1000u)
                                                    : 0);
    cJSON_AddNumberToObject(p, "seq", ps->walk_seq);
    cJSON_AddNumberToObject(p, "tested", ps->walk_tested);
    cJSON_AddNumberToObject(p, "total", ps->walk_total);
    cJSON_AddBoolToObject(p, "life24", (ps->walk_flags & ALARM_WALK_F_LIFE24) != 0);
    cJSON_AddNumberToObject(p, "ended_by", ps->walk_ended_by);
    cJSON_AddStringToObject(p, "ended_by_name", user_name(cfg, ps->walk_ended_by));
    cJSON_AddStringToObject(p, "end_how", ps->walk_end_how < 4 ? WALK_END[ps->walk_end_how] : "");
    cJSON *zones = cJSON_AddArrayToObject(p, "zones"), *live = cJSON_AddArrayToObject(p, "live");
    uint32_t trips = 0;
    bool ok = true;
    for (uint16_t z = 1; z <= snap->zones_used && z <= ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = &cfg->zones[z - 1];
        const alarm_zone_status_t *zs = &snap->zone[z - 1];
        if (zc->type == ALARM_ZT_NOT_USED || zc->partition != (uint8_t)(i + 1))
            continue;
        trips += zs->walk_trips;
        cJSON *v = cJSON_CreateObject();
        cJSON_AddNumberToObject(v, "number", z);
        cJSON_AddStringToObject(v, "name", zc->name); /* as zone-status names it */
        if (!alarm_zone_type_walk_counts(zc->type, ps->walk_flags)) {
            ok &= web_attach(live, NULL, v); /* fire, CO; 24-hour unless included (WALK-007) */
            continue;
        }
        const bool marked = (zs->walk & (ALARM_WALK_ZS_TESTED | ALARM_WALK_ZS_COVER)) != 0;
        cJSON_AddBoolToObject(v, "tested", (zs->walk & ALARM_WALK_ZS_TESTED) != 0);
        cJSON_AddBoolToObject(v, "cover", (zs->walk & ALARM_WALK_ZS_COVER) != 0);
        cJSON_AddStringToObject(v, "how", marked ? WALK_HOW[ALARM_WALK_ZS_KIND(zs->walk) & 3u] : "");
        cJSON_AddNumberToObject(v, "first_s", marked ? zs->walk_first_s : 0);
        cJSON_AddNumberToObject(v, "trips", zs->walk_trips);
        cJSON_AddBoolToObject(v, "open", (zs->flags & ALARM_ZS_FAULT) != 0);
        /* CHECK: a loop or sensor trouble, or missed check-ins (WALK-009) */
        cJSON_AddBoolToObject(v, "check", (zs->flags & (ALARM_ZS_TROUBLE | ALARM_ZS_SUPERVISION)) != 0);
        ok &= web_attach(zones, NULL, v);
    }
    cJSON_AddNumberToObject(p, "trips", trips);
    return built(p, ok);
}
static esp_err_t walk_test_get(httpd_req_t *r, session_t *s, alarm_authority_t authority)
{
    alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (!snap || !cfg || alarm_service_get_config(cfg) != ESP_OK) {
        free(snap);
        free(cfg);
        return error(r, "503 Service Unavailable", "Memory busy");
    }
    alarm_service_get_snapshot(snap);
    const alarm_ms_t now = alarm_service_now();
    const alarm_ms_t age = now > snap->taken_ms ? now - snap->taken_ms : 0;
    /* The partitions this person sees (viewer_partitions(), without reading the configuration
     * again), and only those in use (WALK-018). */
    const uint8_t mine = authority == ALARM_AUTH_INSTALLER || authority == ALARM_AUTH_MASTER
                             ? 0xFF
                             : cfg->users[s->user - 1].partitions;
    const uint8_t seen = (uint8_t)(snap->partitions & mine);
    /* The sums the page counts new marks and repeat trips by, and the key a poll compares: over
     * these partitions only, so nothing of another partition's test shows through them. The key
     * also moves when a test starts, ends or is extended, a zone opens, closes or gets a CHECK,
     * and at a restart (the boot id), so an unchanged answer never hides any of those. */
    uint32_t seq = 0, trips = 0, key = 2166136261u; /* FNV-1a */
#define WALK_KEY(v) (key = (key ^ (uint32_t)(v)) * 16777619u)
    for (const char *c = boot; *c; c++)
        WALK_KEY((unsigned char)*c);
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_partition_status_t *ps = &snap->part[i];
        if (!(seen & (1u << i)))
            continue;
        seq += ps->walk_seq;
        WALK_KEY(i);
        WALK_KEY(ps->walk_test);
        WALK_KEY(ps->walk_seq);
        WALK_KEY(ps->walk_started_ms);
        WALK_KEY(ps->walk_started_ms >> 32);
        WALK_KEY(ps->walk_end_how);
        WALK_KEY(ps->walk_ended_by);
        WALK_KEY(ps->walk_flags);
    }
    for (uint16_t z = 1; z <= snap->zones_used && z <= ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = &cfg->zones[z - 1];
        const alarm_zone_status_t *zs = &snap->zone[z - 1];
        if (zc->type == ALARM_ZT_NOT_USED || zc->partition < 1 ||
            zc->partition > ALARM_MAX_PARTITIONS || !(seen & (1u << (zc->partition - 1))))
            continue;
        trips += zs->walk_trips;
        WALK_KEY(z);
        WALK_KEY(zs->walk);
        WALK_KEY(zs->walk_trips);
        WALK_KEY(zs->flags & (ALARM_ZS_FAULT | ALARM_ZS_TROUBLE | ALARM_ZS_SUPERVISION));
    }
#undef WALK_KEY
    uint32_t was;
    const bool unchanged = query_number(r, "key", &was) && was == key;
    cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
    bool ok = true;
    if (unchanged)
        cJSON_AddBoolToObject(o, "unchanged", true);
    cJSON_AddNumberToObject(o, "key", key);
    cJSON_AddNumberToObject(o, "seq", seq);
    cJSON_AddNumberToObject(o, "trips", trips);
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if (!(seen & (1u << i)))
            continue;
        if (unchanged) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "number", i + 1);
            cJSON_AddBoolToObject(v, "running", snap->part[i].walk_test);
            cJSON_AddNumberToObject(v, "remaining_s", walk_left_s(&snap->part[i], age));
            ok &= web_attach(a, NULL, v);
        } else {
            ok &= web_attach(a, NULL, walk_partition(snap, cfg, i, now, age));
        }
    }
    ok &= web_attach(o, "partitions", a);
    mbedtls_platform_zeroize(cfg, sizeof(*cfg)); /* it holds the code hashes too */
    free(cfg);
    free(snap);
    return json(r, built(o, ok));
}
static cJSON *status(session_t *s, alarm_authority_t authority)
{
    cJSON *o = cJSON_CreateObject(), *caps = cJSON_CreateObject(),
          *integrations = cJSON_CreateArray();
    bool ok = true;
    cJSON_AddStringToObject(o, "panel_id", identity);
    cJSON_AddStringToObject(o, "boot", boot);
    cJSON_AddStringToObject(o, "board", A.board);
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "csrf", s->csrf);
    cJSON_AddNumberToObject(o, "user", s->user);
    cJSON_AddStringToObject(o, "authority", alarm_authority_name(authority));
    cJSON_AddBoolToObject(o, "developer", atomic_load(&dev_on));
    cJSON_AddBoolToObject(o, "web", atomic_load(&web_on));
    cJSON_AddBoolToObject(o, "network_pending", net_wifi_trial_pending());
    cJSON_AddBoolToObject(o, "has_password", web_login_has(s->user));
    cJSON_AddBoolToObject(o, "password_change_required", must_change(s));
    /* Seconds this session's recent password still stands for the guarded changes; 0: the next
     * one asks for it (recent_password()). */
    cJSON_AddNumberToObject(o, "reauth_s", (double)(password_left_us(s) / 1000000));
    /* What this user may open: the browser shows these pages only (the server checks anyway). */
    cJSON *access = cJSON_CreateObject();
    cJSON_AddBoolToObject(access, "users", allowed(authority, ACC_USERS));
    cJSON_AddBoolToObject(access, "shared", allowed(authority, ACC_SHARED));
    cJSON_AddBoolToObject(access, "system", allowed(authority, ACC_SYSTEM));
    cJSON_AddBoolToObject(access, "installer", allowed(authority, ACC_INSTALLER));
    /* Arming and disarming: never the installer's (BL-187), anyone else's who has a code. */
    cJSON_AddBoolToObject(access, "arm",
                          authority != ALARM_AUTH_NONE && authority != ALARM_AUTH_INSTALLER);
    ok &= web_attach(o, "access", access);
    /* An update runs (Install to the end of the self-test), for every page to say so (BL-188).
     * No memory to read it just now: false, the next poll tells. */
    cJSON_AddBoolToObject(o, "updating", ota_running_now() == 1);
    cJSON_AddNumberToObject(caps, "width", A.width);
    cJSON_AddNumberToObject(caps, "height", A.height);
    cJSON_AddBoolToObject(caps, "jpeg", A.jpeg != NULL);
    cJSON_AddNumberToObject(caps, "frame_interval_ms",
                            A.frame_interval_ms ? A.frame_interval_ms : 250);
    cJSON_AddBoolToObject(caps, "input", A.input != NULL);
    /* The remote keypad's WebSocket (web_screen.c): which pictures this board streams. */
    cJSON *video = cJSON_CreateArray();
    if (A.video && (A.video_codecs & DEV_VIDEO_H264))
        ok &= web_attach(video, NULL, cJSON_CreateString("h264"));
    if (A.video && (A.video_codecs & DEV_VIDEO_JPEG))
        ok &= web_attach(video, NULL, cJSON_CreateString("jpeg"));
    ok &= web_attach(caps, "video", video);
    {
        ui_zw_status_t zw; /* the Z-Wave page (installer): only on a panel whose radio runs */
        cJSON_AddBoolToObject(caps, "zwave", B.zw_status != NULL && B.zw_status(B.ctx, &zw));
        /* The Zigbee page (installer, devices.md §14): whenever this panel runs the Zigbee
         * service, an empty socket included (the page says why) */
        ui_zb_status_t zb;
        cJSON_AddBoolToObject(caps, "zigbee", B.zb_status != NULL && B.zb_status(B.ctx, &zb));
        /* The Automations page (system master, installer: docs/features/41 DESIGN.md section 10) */
        cJSON_AddBoolToObject(caps, "automations", web_automations_available());
    }
    if (B.halink_status)
        ok &= web_attach(integrations, NULL, cJSON_CreateString("home_assistant"));
    if (B.mqtt_get)
        ok &= web_attach(integrations, NULL, cJSON_CreateString("mqtt"));
    if (B.ntfy_get)
        ok &= web_attach(integrations, NULL, cJSON_CreateString("ntfy"));
    if (B.pushover_get)
        ok &= web_attach(integrations, NULL, cJSON_CreateString("pushover"));
    ok &= web_attach(caps, "integrations", integrations);
    ok &= web_attach(o, "capabilities", caps);
    ui_wifi_info_t w;
    B.wifi_info(B.ctx, &w);
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", w.ssid);
    cJSON_AddStringToObject(wifi, "ip", w.ip);
    cJSON_AddNumberToObject(wifi, "rssi", w.rssi);
    cJSON_AddBoolToObject(wifi, "time_synced", w.time_synced);
    ok &= web_attach(o, "wifi", wifi);
    ok &= add_clock(o);
    ui_about_t about;
    B.about(B.ctx, &about);
    cJSON *health = cJSON_CreateObject();
    cJSON_AddNumberToObject(health, "uptime_s", about.uptime_s);
    cJSON_AddNumberToObject(health, "internal_kb", about.heap_internal_kb);
    cJSON_AddNumberToObject(health, "psram_kb", about.heap_psram_kb);
    cJSON_AddNumberToObject(health, "stale", panel_ota_health_stale());
    cJSON_AddNumberToObject(health, "internal_largest_kb",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    /* Unused stack of the HTTPS task serving this request (its stack is internal RAM). */
    cJSON_AddNumberToObject(health, "web_stack_free", uxTaskGetStackHighWaterMark(NULL));
    cJSON_AddStringToObject(health, "chip", about.chip);
    cJSON_AddStringToObject(health, "idf", about.idf);
    if (B.power_info) {
        ui_power_info_t p;
        B.power_info(B.ctx, &p);
        cJSON *power = cJSON_CreateObject();
        cJSON_AddStringToObject(power, "battery",
                                p.state == UI_BATTERY_PRESENT  ? "present"
                                : p.state == UI_BATTERY_ABSENT ? "absent"
                                                               : "unknown");
        cJSON_AddNumberToObject(power, "percent", p.percent);
        cJSON_AddBoolToObject(power, "low", p.low);
        cJSON_AddBoolToObject(power, "charging", p.charging);
        ok &= web_attach(health, "power", power);
    }
    if (A.metrics) {
        char m[256];
        A.metrics(m, sizeof(m));
        cJSON *display = cJSON_Parse(m);
        if (display)
            ok &= web_attach(health, "display", display);
    }
    ok &= web_attach(o, "health", health); /* last: health is freed if it can't go in */
    alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (snap && cfg && alarm_service_get_config(cfg) == ESP_OK) {
        alarm_service_get_snapshot(snap);
        cJSON_AddStringToObject(o, "name", s->user == ALARM_USER_INSTALLER
                                               ? "Installer"
                                               : cfg->users[s->user - 1].name);
        cJSON_AddNumberToObject(o, "config_revision", snap->config_seq);
        cJSON_AddBoolToObject(o, "program", snap->program);
        cJSON_AddBoolToObject(o, "locked", alarm_snapshot_locked(snap));
        cJSON_AddNumberToObject(o, "troubles", snap->sys_troubles);
        /* The partitions this person sees (viewer_partitions(), without reading the
         * configuration again). */
        const uint8_t mine = authority == ALARM_AUTH_INSTALLER || authority == ALARM_AUTH_MASTER
                                 ? 0xFF
                                 : cfg->users[s->user - 1].partitions;
        /* The touchscreen's Troubles list, active and restored (UI-030): GET troubles has it. */
        cJSON_AddNumberToObject(o, "trouble_count",
                                (double)alarm_snapshot_troubles(snap, cfg, mine, NULL, 0));
        /* Changes when a line is restored too, which keeps the count (BL-137). */
        cJSON_AddNumberToObject(o, "trouble_sig",
                                (double)alarm_snapshot_troubles_sig(snap, cfg, mine));
        const alarm_ms_t now = alarm_service_now();
        const alarm_ms_t age = now > snap->taken_ms ? now - snap->taken_ms : 0;
        cJSON *parts = cJSON_CreateArray();
        for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++)
            if (snap->partitions & mine & (1u << i))
                ok &= web_attach(parts, NULL, partition_status(snap, cfg, i, age));
        ok &= web_attach(o, "partitions", parts);
        /* An automation's coming arming, or its hold after a put-off, on those partitions: the
         * Overview's card and note for everyone who sees them (docs/features/41 D19, D20, D27),
         * as Home shows them at the panel. */
        ok &= web_automations_add_warnings(o, "automation_warnings", cfg, snap->partitions & mine);
    }
    free(snap);
    if (cfg) {
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
    }
    return built(o, ok);
}
/* ---- languages (docs/features/37 language-packs.md "The Panel Manager", I18N-046) -------------
 * GET /api/v1/languages: what is installed, what the update source offers, the last download.
 * POST {"action": "remove"|"download", "locale"}. PUT: the body is a .robolang file. */
static cJSON *languages_get(void)
{
    cJSON *o = cJSON_CreateObject();
    bool ok = true;
    panel_prefs_t prefs;
    panel_prefs_load(&prefs);
    cJSON_AddStringToObject(o, "current", prefs.lang[0] ? prefs.lang : "en-US");
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "free", lang_packs_free_bytes());
    static const char *const states[] = {"ready", "other", "refused", "damaged"};
    cJSON *in = cJSON_AddArrayToObject(o, "installed");
    lang_pack_info_t *list = web_alloc(LANG_PACKS_LIST_MAX * sizeof(*list));
    if (list) {
        const size_t n = lang_packs_list(list, LANG_PACKS_LIST_MAX);
        for (size_t i = 0; i < n; i++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "locale", list[i].locale);
            cJSON_AddStringToObject(v, "name", list[i].name);
            cJSON_AddStringToObject(v, "version", list[i].version);
            cJSON_AddBoolToObject(v, "draft", list[i].draft);
            cJSON_AddStringToObject(v, "state", states[list[i].state & 3]);
            cJSON_AddNumberToObject(v, "missing", list[i].missing);
            cJSON_AddNumberToObject(v, "size", list[i].size);
            cJSON_AddBoolToObject(v, "loaded", list[i].loaded);
            cJSON_AddBoolToObject(v, "default", list[i].is_default);
            ok &= web_attach(in, NULL, v);
        }
        free(list);
    }
    cJSON *av = cJSON_AddArrayToObject(o, "available");
    lang_available_t *offer = web_alloc(LANG_PACKS_LIST_MAX * sizeof(*offer));
    if (offer) {
        const size_t n = lang_packs_available(offer, LANG_PACKS_LIST_MAX);
        for (size_t i = 0; i < n; i++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "locale", offer[i].locale);
            cJSON_AddStringToObject(v, "version", offer[i].version);
            cJSON_AddNumberToObject(v, "size", offer[i].size);
            ok &= web_attach(av, NULL, v);
        }
        free(offer);
    }
    lang_fetch_status_t fs;
    lang_packs_fetch_status(&fs);
    static const char *const phases[] = {"idle", "downloading", "done", "failed"};
    cJSON *f = cJSON_AddObjectToObject(o, "fetch");
    cJSON_AddStringToObject(f, "locale", fs.locale);
    cJSON_AddStringToObject(f, "phase", phases[fs.phase & 3]);
    cJSON_AddStringToObject(f, "result", lang_result_name((lang_result_t)fs.result));
    return built(o, ok);
}
/* A result's status: 200 ok, 409 locked, 503 busy or a flash fault, else 400. */
static esp_err_t language_answer(httpd_req_t *r, lang_result_t why)
{
    const char *status = "400 Bad Request";
    if (why == LANG_OK)
        status = "200 OK";
    else if (why == LANG_ERR_LOCKED)
        status = "409 Conflict";
    else if (why == LANG_ERR_BUSY || why == LANG_ERR_FLASH)
        status = "503 Service Unavailable";
    httpd_resp_set_status(r, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", lang_result_name(why));
    return json(r, o);
}
static esp_err_t languages_post(httpd_req_t *r, const cJSON *o)
{
    const char *action = str(o, "action"), *locale = str(o, "locale");
    const bool remove = !strcmp(action, "remove");
    if ((!remove && strcmp(action, "download")) || !i18n_tag_valid(locale))
        return error(r, "400 Bad Request", "Unknown language action");
    return language_answer(r, remove ? lang_packs_remove(locale) : lang_packs_fetch(locale));
}
static esp_err_t languages_put(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    if (!s)
        return error(r, "401 Unauthorized", "Sign in again or reload the page");
    if (must_change(s) || !allowed(authority, ACC_SHARED))
        return error(r, "403 Forbidden", "not_allowed");
    if (panel_locked())
        return error(r, "409 Conflict", "locked");
    if (r->content_len < 8192 || r->content_len > 2u * 1024 * 1024 || r->content_len % 4096 != 0) {
        if (r->content_len > 0)
            httpd_resp_set_hdr(r, "Connection", "close"); /* the body stays unread */
        s_body_unread = false;
        httpd_resp_set_status(r, "400 Bad Request");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "result", lang_result_name(LANG_ERR_SIZE));
        (void)json(r, o);
        return r->content_len > 0 ? ESP_FAIL : ESP_OK;
    }
    uint8_t *file = heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM);
    if (!file)
        return error(r, "503 Service Unavailable", "Memory busy");
    size_t got = 0;
    const int64_t deadline = esp_timer_get_time() + 60000000;
    while (got < r->content_len && esp_timer_get_time() < deadline) {
        const int n = httpd_req_recv(r, (char *)file + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    if (got == r->content_len)
        s_body_unread = false;
    /* This task's stack is internal RAM, as the install (it writes flash) needs. */
    const lang_result_t why = got == r->content_len ? lang_packs_install(file, got) : LANG_ERR_SIZE;
    heap_caps_free(file);
    return language_answer(r, why);
}
/* ---- the voice pack (docs/features/08 voice-first-build.md D12, D14; docs/features/31) ----------
 * GET /api/v1/voices: web_voice_get(). PUT: the body is a signed .robovoice, the system master's
 * and the installer's, only while disarmed and quiet; every answer is {"result"} (web_voice.h).
 *
 * The upload never holds the HTTPS worker (code review finding 1). voices_put() checks the sign-in,
 * the state and the size on the worker as before, then hands the request to one upload task
 * (httpd_req_async_handler_begin()) and returns at once. Why arming, disarming and the Remote
 * keypad stay responsive meanwhile, even while an upload stalls:
 * - the server's task is back in select() as soon as voices_put() returns: the upload's socket is
 *   marked for_async_req, which takes it out of the server's select set and out of the LRU
 *   purge, so nothing the server does waits for it;
 * - the upload holds one of the four sockets (max_open_sockets = 4, no change needed): the pages
 *   and the keypad's WebSocket keep the other three, and an idle one is purged for a new
 *   connection (lru_purge_enable);
 * - the upload task runs below the server's priority (1 against 2): receiving and checking a
 *   970 KB pack never delays a request the server has to answer;
 * - one upload at a time (s_voice_busy): a second is answered "busy" on the worker, at once,
 *   its body unread and its connection closed, as before.
 * The task's stack is internal RAM (xTaskCreate), as panel_voice_install() needs for its flash
 * writes; it exists only while an upload runs (12 KB) and deletes itself. It keeps the body's
 * state for its own request (got against content_len), checks the sign-in again on the server's
 * task just before installing (the session table is that task's), closes a connection whose body
 * it didn't read in full, and completes the request on every way out. maintenance() waits for it
 * before it stops the server, whose request it holds (voice_drain()). The firmware isn't on a
 * host harness: this is proven by reading; the answers are the page tests' (panel_pages.test.cjs). */
#define VOICE_UPLOAD_STACK 12288
#define VOICE_UPLOAD_PRIO  1 /* below the HTTPS server's task_priority 2 */
typedef struct {
    httpd_req_t *req; /* the async copy: this task's until it completes it */
    uint8_t *file;
    char token[65]; /* the sign-in it was accepted under, checked again before installing */
    uint8_t user;
} voice_upload_t;
static voice_upload_t s_voice_upload; /* the one upload in flight: s_voice_busy owns it */
static atomic_bool s_voice_busy, s_voice_stop;
typedef enum { VC_OK, VC_SIGNED_OUT, VC_NOT_ALLOWED, VC_MEMORY } voice_check_t;
static atomic_uint s_voice_check_seq;
static atomic_int s_voice_check_answer;
static SemaphoreHandle_t s_voice_check_done; /* made once, on the server's task */
static esp_err_t voice_result(httpd_req_t *r, const char *status, const char *result)
{
    httpd_resp_set_status(r, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", result);
    if (s_body_unread && r->content_len > 0) {
        httpd_resp_set_hdr(r, "Connection", "close"); /* the body stays unread */
        (void)json(r, o);
        s_body_unread = false;
        return ESP_FAIL;
    }
    return json(r, o);
}
/* The upload task's own answers: nothing of the server task's (s_body_unread, error()). */
static void voice_reply(httpd_req_t *r, const char *status, const char *key, const char *word, bool close)
{
    httpd_resp_set_status(r, status);
    if (close)
        httpd_resp_set_hdr(r, "Connection", "close");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, key, word);
    (void)json(r, o);
}
/* Every way out of an accepted upload: a body not read in full closes the connection (the
 * server would otherwise read what is left as the next request); the request is completed, which
 * gives its socket back to the server's select(). The close is queued (it runs on the server's
 * task) before the socket is given back, so the server never reads the rest of the body as a
 * request; complete() touches only the copy and the socket's slot, never the closed TLS session. */
static void voice_upload_end(httpd_req_t *r, bool close)
{
    if (close)
        (void)httpd_sess_trigger_close(r->handle, httpd_req_to_sockfd(r));
    (void)httpd_req_async_handler_complete(r);
}
/* On the server's task (httpd_queue_work): is the sign-in the upload was accepted under still
 * open, the same person, and still allowed to install? As session() and voices_put() decide. */
static void voice_check_work(void *arg)
{
    if ((unsigned)(uintptr_t)arg != atomic_load(&s_voice_check_seq))
        return; /* a check the upload stopped waiting for */
    voice_check_t answer = VC_SIGNED_OUT;
    const int64_t now = esp_timer_get_time();
    for (unsigned i = 0; atomic_load(&web_on) && i < SESSION_COUNT; i++) {
        session_t *s = &sessions[i];
        if (!s->token[0] || !equal(s->token, s_voice_upload.token, 64) || s->user != s_voice_upload.user)
            continue;
        uint8_t proof[32];
        alarm_authority_t authority;
        if (now - s->used > SESSION_IDLE_US || now - s->created > SESSION_MAX_US)
            break;
        if (!user_proof(s->user, proof, &authority, NULL))
            answer = VC_MEMORY; /* couldn't be computed just now: not a mismatch */
        else if (!equal(proof, s->proof, 32))
            break; /* the code changed: the sign-in is over (session() ends it next time) */
        else
            answer = must_change(s) || !allowed(authority, ACC_SYSTEM) ? VC_NOT_ALLOWED : VC_OK;
        break;
    }
    atomic_store(&s_voice_check_answer, (int)answer);
    xSemaphoreGive(s_voice_check_done);
}
static voice_check_t voice_check(httpd_handle_t hd)
{
    const unsigned seq = atomic_fetch_add(&s_voice_check_seq, 1) + 1;
    (void)xSemaphoreTake(s_voice_check_done, 0); /* a late answer to an earlier check */
    if (httpd_queue_work(hd, voice_check_work, (void *)(uintptr_t)seq) != ESP_OK ||
        xSemaphoreTake(s_voice_check_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        atomic_fetch_add(&s_voice_check_seq, 1); /* never mind it now */
        return VC_MEMORY;
    }
    return (voice_check_t)atomic_load(&s_voice_check_answer);
}
static void voice_upload_task(void *arg)
{
    voice_upload_t *u = arg;
    httpd_req_t *r = u->req;
    size_t got = 0;
    const int64_t deadline = esp_timer_get_time() + 60000000;
    while (got < r->content_len && esp_timer_get_time() < deadline && !atomic_load(&s_voice_stop)) {
        const int n = httpd_req_recv(r, (char *)u->file + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue; /* at most recv_wait_timeout (3 s): the deadline and a stop are seen */
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    const bool whole = got == r->content_len;
    if (!whole) {
        voice_reply(r, "400 Bad Request", "result", "too_big", true); /* cut short: not the size it said */
    } else {
        switch (voice_check(r->handle)) {
        case VC_OK: {
            /* This task's stack is internal RAM, as the install (it writes flash) needs. */
            const panel_voice_install_t why = panel_voice_install(u->file, got);
            const char *status = "400 Bad Request";
            const char *result = web_voice_answer(why, &status);
            ESP_LOGI("panel_web", "user %u voice pack install: %s", (unsigned)u->user,
                     panel_voice_install_name(why));
            voice_reply(r, status, "result", result, false);
            break;
        }
        case VC_NOT_ALLOWED:
            voice_reply(r, "403 Forbidden", "result", "not_allowed", false);
            break;
        case VC_MEMORY:
            voice_reply(r, "503 Service Unavailable", "error", "Memory busy", false);
            break;
        default:
            voice_reply(r, "401 Unauthorized", "error", "Sign in again or reload the page", false);
            break;
        }
    }
    heap_caps_free(u->file);
    voice_upload_end(r, !whole);
    mbedtls_platform_zeroize(u, sizeof(*u));
    atomic_store(&s_voice_busy, false);
    vTaskDelete(NULL);
}
/* maintenance(), before it stops the server: no new upload starts, and the one in flight (it
 * holds a request of this server) ends - within 3 s while receiving, else when its install does.
 * s_voice_stop stays set until the server has stopped: voice_drain_end(). */
static void voice_drain(void)
{
    atomic_store(&s_voice_stop, true);
    while (atomic_load(&s_voice_busy))
        vTaskDelay(pdMS_TO_TICKS(100));
}
static void voice_drain_end(void)
{
    atomic_store(&s_voice_stop, false);
}
static esp_err_t voices_put(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    if (!s)
        return error(r, "401 Unauthorized", "Sign in again or reload the page");
    if (must_change(s) || !allowed(authority, ACC_SYSTEM))
        return voice_result(r, "403 Forbidden", "not_allowed");
    if (panel_locked())
        return voice_result(r, "409 Conflict", "armed");
    if (r->content_len < VP_FILE_MIN || r->content_len > VP_FILE_MAX || r->content_len % VP_SECTOR != 0)
        return voice_result(r, "400 Bad Request", "too_big"); /* before reading a byte */
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_voice_busy, &idle, true))
        return voice_result(r, "409 Conflict", "busy"); /* one upload at a time */
    if (atomic_load(&s_voice_stop)) {
        atomic_store(&s_voice_busy, false);
        return voice_result(r, "409 Conflict", "busy"); /* the server is stopping */
    }
    voice_upload_t *u = &s_voice_upload;
    *u = (voice_upload_t){.user = s->user};
    memcpy(u->token, s->token, sizeof(u->token));
    if (!s_voice_check_done)
        s_voice_check_done = xSemaphoreCreateBinary();
    u->file = s_voice_check_done ? heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM) : NULL;
    if (!u->file || httpd_req_async_handler_begin(r, &u->req) != ESP_OK) {
        heap_caps_free(u->file);
        mbedtls_platform_zeroize(u, sizeof(*u));
        atomic_store(&s_voice_busy, false);
        return error(r, "503 Service Unavailable", "Memory busy");
    }
    s_body_unread = false; /* the body is the copy's now: begin() left this request none to read */
    if (xTaskCreate(voice_upload_task, "web_voice", VOICE_UPLOAD_STACK, u, VOICE_UPLOAD_PRIO, NULL) !=
        pdPASS) {
        voice_reply(u->req, "503 Service Unavailable", "error", "Memory busy", true);
        voice_upload_end(u->req, true);
        heap_caps_free(u->file);
        mbedtls_platform_zeroize(u, sizeof(*u));
        atomic_store(&s_voice_busy, false);
    }
    return ESP_OK;
}
static esp_err_t get(httpd_req_t *r)
{
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    char path[96]; /* /api/v1/<path>, without any query string */
    api_path(r, path, sizeof(path));
    /* The setup pages' status, for anyone who reaches the panel (nothing secret in it,
     * web_setup.h); while nobody has claimed a headless panel, nothing else answers. */
    if (!strcmp(path, "setup"))
        return json(r, web_setup_status(via_setup_wifi(r), s ? s->user : 0));
    if (web_setup_unclaimed())
        return error(r, "403 Forbidden", "setup_first");
    if (!s)
        return error(r, "401 Unauthorized", "Sign in to this panel");
    if (!strcmp(path, "status"))
        return json(r, status(s, authority));
    if (must_change(s))
        return error(r, "403 Forbidden", "password_change_required");
    if (!allowed(authority, get_access(path)))
        return error(r, "403 Forbidden", "not_allowed");
    if (!strcmp(path, "developer/logs")) {
        if (!atomic_load(&dev_on))
            return error(r, "404 Not Found", "Developer mode is disabled");
        char *text = web_alloc(16384);
        if (!text)
            return error(r, "503 Service Unavailable", "Memory busy");
        dev_remote_copy_log(text, 16384);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "text", text);
        free(text);
        return json(r, o);
    }
    if (!strcmp(path, "frame")) {
        if (!A.jpeg)
            return error(r, "404 Not Found", "No capture adapter");
        int64_t now = esp_timer_get_time();
        if (now < next_frame)
            return error(r, "429 Too Many Requests", "Frame rate limit");
        net_wifi_note_interaction();
        next_frame = now + (int64_t)(A.frame_interval_ms ? A.frame_interval_ms : 250) * 1000;
        const uint8_t *data = NULL;
        size_t length = 0;
        esp_err_t e = A.jpeg(&data, &length);
        if (e != ESP_OK)
            return error(r, "503 Service Unavailable", "Display is busy");
        headers(r);
        httpd_resp_set_type(r, "image/jpeg");
        httpd_resp_set_hdr(r, "X-Panel-Boot", boot);
        char timing[48];
        snprintf(timing, sizeof(timing), "capture;dur=%.1f", (esp_timer_get_time() - now) / 1000.0);
        httpd_resp_set_hdr(r, "Server-Timing", timing);
        return httpd_resp_send(r, (const char *)data, length);
    }
    if (!strcmp(path, "zwave")) /* the installer's (get_access): docs/features/36 screens.md §11 */
        return json(r, web_zwave_get(&B));
    /* ACC_SYSTEM (get_access): docs/features/41 DESIGN.md section 10. Read-only while locked: the
     * page shows the armed notice, every edit answers 409 locked. */
    if (!strcmp(path, "automations") || !strcmp(path, "automations/devices")) {
        if (!web_automations_available())
            return error(r, "404 Not Found", "Automations are not in this firmware");
        cJSON *o = !strcmp(path, "automations") ? web_automations_get() : web_automations_devices();
        return o ? json(r, o) : error(r, "503 Service Unavailable", "Memory busy");
    }
    if (!strcmp(path, "zigbee")) { /* the installer's (get_access): devices.md §14, ZB-025 */
        uint32_t device = 0;
        char q[64];
        const bool asked = httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
                           strstr(q, "device=") != NULL;
        bool found = true;
        cJSON *o = web_zigbee_get(&B, query_number(r, "device", &device) ? device : 0,
                                  !panel_locked(), &found);
        if (asked && (device == 0 || !found)) { /* no such device (removed since), or not a number */
            cJSON_Delete(o);
            return error(r, "404 Not Found", "not_found");
        }
        return o ? json(r, o) : error(r, "503 Service Unavailable", "Memory busy");
    }
    if (!strcmp(path, "languages"))
        return json(r, languages_get());
    if (!strcmp(path, "voices")) {
        cJSON *o = web_voice_get();
        return o ? json(r, o) : error(r, "404 Not Found", "Voice is not in this firmware");
    }
    if (!strncmp(path, "config/", 7)) {
        bool busy = false;
        cJSON *o = web_config_get(&B, path + 7, request_words(r), &busy);
        return o      ? json(r, o)
               : busy ? error(r, "503 Service Unavailable", "Memory busy")
                      : error(r, "404 Not Found", "Configuration is not included in this firmware");
    }
    if (!strcmp(path, "alarm") || !strcmp(path, "people")) {
        alarm_config_t *cfg = web_alloc(sizeof(*cfg));
        if (!cfg || alarm_service_get_config(cfg) != ESP_OK) {
            free(cfg);
            return error(r, "503 Service Unavailable", "Configuration busy");
        }
        cJSON *o;
        bool ok = true;
        if (!strcmp(path, "alarm")) {
            o = web_alarm_config_get(cfg);
            alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
            if (snap) {
                alarm_service_get_snapshot(snap);
                cJSON_AddNumberToObject(o, "revision", snap->config_seq);
                free(snap);
            }
        } else {
            /* The users this person sees on the panel's Users page: everyone for the installer
             * and the system master, a partition master its own partitions' people. */
            o = cJSON_CreateObject();
            cJSON *a = cJSON_CreateArray();
            const alarm_user_t *me = &cfg->users[s->user - 1];
            for (unsigned i = 0; i < ALARM_MAX_USERS; i++) {
                const alarm_user_t *u = &cfg->users[i];
                const uint8_t n = (uint8_t)(i + 1);
                if (u->authority == ALARM_AUTH_NONE ||
                    (me->authority == ALARM_AUTH_PARTITION_MASTER && n != s->user &&
                     (u->partitions & me->partitions) == 0))
                    continue;
                cJSON *v = cJSON_CreateObject();
                cJSON_AddNumberToObject(v, "number", n);
                cJSON_AddStringToObject(v, "name", u->name);
                cJSON_AddNumberToObject(v, "authority", u->authority);
                cJSON_AddNumberToObject(v, "partitions", u->partitions);
                cJSON_AddBoolToObject(v, "you", n == s->user);
                /* May the signed-in person change this user (code, name, sign-in)? The engine's
                 * own rule; it checks again on every change (CORE-USR-028). */
                cJSON_AddBoolToObject(v, "manage", alarm_config_user_manages(cfg, s->user, n));
                cJSON_AddBoolToObject(v, "web_login", web_login_enabled(cfg, n));
                cJSON_AddBoolToObject(v, "web_login_possible", web_login_eligible(cfg, n));
                cJSON_AddBoolToObject(v, "web_login_temporary", web_login_temporary(n));
                ok &= web_attach(a, NULL, v);
            }
            ok &= web_attach(o, "users", a);
            cJSON_AddNumberToObject(o, "maximum", ALARM_MAX_USERS);
            cJSON_AddStringToObject(o, "panel_id", web_login_panel_id());
            /* The fewest digits a new code may have (the installer's option, never under the
             * engine's own minimum): the page says it before the panel refuses one. */
            cJSON_AddNumberToObject(o, "code_min", cfg->opt.code_min_len > ALARM_CODE_MIN_LEN
                                                       ? cfg->opt.code_min_len
                                                       : ALARM_CODE_MIN_LEN);
            /* A user added here gets a free slot above the system master. */
            cJSON_AddBoolToObject(o, "can_add", me->authority == ALARM_AUTH_INSTALLER ||
                                                    me->authority == ALARM_AUTH_MASTER ||
                                                    me->authority == ALARM_AUTH_PARTITION_MASTER);
        }
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
        return json(r, built(o, ok));
    }
    /* "history", not "activity": EasyPrivacy (on by default in uBlock Origin, AdGuard and
     * Brave) blocks every same-site request to a URL ending in /api/v1/activity, and the
     * browser reports only "Failed to fetch" (docs/bugs/B007). The old name stays for scripts. */
    if (!strcmp(path, "history") || !strcmp(path, "activity")) {
        /* Only the events of this person's partitions, read for this request (WEB-016, code
         * review S06): web_history_visible() says which. Both names, one answer. */
        uint8_t mine;
        if (!viewer_partitions(s->user, authority, &mine))
            return error(r, "503 Service Unavailable", "Memory busy");
        elog_entry_t *entries = web_alloc(32 * sizeof(*entries));
        alarm_config_t *cfg = web_alloc(sizeof(*cfg));
        if (!entries || !cfg || alarm_service_get_config(cfg) != ESP_OK) {
            free(entries);
            free(cfg);
            return error(r, "503 Service Unavailable", "Memory busy");
        }
        uint32_t next;
        web_history_view_t view = {.cfg = cfg, .mine = mine};
        size_t count = panel_log_read(UINT32_MAX, web_history_keep, &view, entries, 32, 128, &next);
        cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
        bool ok = true;
        for (size_t i = 0; i < count; i++) {
            if (entries[i].kind != ELOG_KIND_EVENT)
                continue;
            alarm_event_t *e = &entries[i].ev;
            notify_words_t words;
            if (!notify_words(request_words(r), cfg, e, false, &words))
                continue;
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "title", words.title);
            cJSON_AddStringToObject(v, "body", words.body);
            cJSON_AddNumberToObject(v, "index", entries[i].index);
            cJSON_AddNumberToObject(v, "boot", entries[i].boot);
            cJSON_AddNumberToObject(v, "uptime_s", entries[i].uptime_s);
            cJSON_AddNumberToObject(v, "epoch_ms", (double)e->epoch_ms); /* 0 = clock unknown */
            cJSON_AddNumberToObject(v, "category", words.cat);
            cJSON_AddNumberToObject(v, "accent", words.accent);
            cJSON_AddStringToObject(v, "event", alarm_event_name(e->type));
            cJSON_AddNumberToObject(v, "zone", e->zone);
            cJSON_AddNumberToObject(v, "partition", e->partition);
            cJSON_AddNumberToObject(v, "user", e->user);
            ok &= web_attach(a, NULL, v);
        }
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
        free(entries);
        ok &= web_attach(o, "events", a);
        return json(r, built(o, ok));
    }
    if (!strcmp(path, "time")) {
        char tz[96], ntp[128];
        B.get_timezone(B.ctx, tz, sizeof(tz));
        B.get_ntp(B.ctx, ntp, sizeof(ntp));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "timezone", tz);
        cJSON_AddStringToObject(o, "ntp", ntp);
        cJSON_AddNumberToObject(o, "now_ms", (double)B.epoch_ms(B.ctx)); /* 0 = not set */
        const bool ok = add_clock(o);
        return json(r, built(o, ok));
    }
    if (!strcmp(path, "home_assistant") && B.halink_status) {
        ui_halink_status_t h;
        B.halink_status(B.ctx, &h);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", h.enabled);
        cJSON_AddBoolToObject(o, "paired", h.paired);
        cJSON_AddNumberToObject(o, "state", h.state);
        cJSON_AddStringToObject(o, "client", h.client);
        cJSON_AddStringToObject(o, "code", h.code);
        cJSON_AddNumberToObject(o, "window_s", h.window_s);
        return json(r, o);
    }
    if (!strcmp(path, "keypads"))
        return json(r, web_keypads_get(s->user));
    if (!strcmp(path, "updates")) /* ACC_SYSTEM (get_access): OTA-022 */
        return updates_get(r, authority);
    if (!strcmp(path, "walk-test")) /* ACC_SHARED (get_access): docs/features/40 */
        return walk_test_get(r, s, authority);
    uint8_t mine;
    if (!viewer_partitions(s->user, authority, &mine))
        return error(r, "503 Service Unavailable", "Memory busy");
    cJSON *extra = web_extra_get(&B, path, mine, request_words(r));
    if (extra && memory_busy(extra))
        httpd_resp_set_status(r, "503 Service Unavailable");
    return extra ? json(r, extra) : error(r, "404 Not Found", "Not included in this firmware");
}
/* A browser sign-in for another user, by whoever manages them: set a password or turn the
 * sign-in off (never your own: POST people refuses that). temporary: they replace it at their
 * first sign-in, if the person setting it chose so. */
static esp_err_t people_login(httpd_req_t *r, session_t *s, uint8_t user, bool on,
                              const char *password, bool temporary)
{
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (!cfg || alarm_service_get_config(cfg) != ESP_OK) {
        free(cfg);
        return error(r, "503 Service Unavailable", "Configuration busy");
    }
    const bool may = alarm_config_user_manages(cfg, s->user, user);
    const bool eligible = web_login_eligible(cfg, user);
    mbedtls_platform_zeroize(cfg, sizeof(*cfg));
    free(cfg);
    if (!may)
        return result(r, ALARM_ERR_NOT_AUTHORIZED);
    char why[128] = "";
    if (!on) {
        /* Refused by flash: the sign-in is off already and its sessions over, but not for good
         * until a retry gets through (web_login.h): say so, never "done" (code review S01). */
        if (!web_login_clear(user)) {
            /* web_login.c's answer, marked there: said() translates it. */
            snprintf(why, sizeof(why), "%s", "The panel could not store it. Try again");
            return error(r, "503 Service Unavailable", said(r, why, sizeof(why)));
        }
    } else if (!eligible) {
        return error(r, "400 Bad Request",
                     i18n_text(request_words(r), "why",
                               "Arm-only, guest and duress codes can't have a browser sign-in"));
    } else if (!web_login_set(user, password, temporary, why, sizeof(why))) {
        /* Flash or memory, not the person: 503, as a refused turn-off. Decided before said()
         * translates why. */
        const char *status =
            web_login_why_busy(why) ? "503 Service Unavailable" : "400 Bad Request";
        return error(r, status, said(r, why, sizeof(why)));
    }
    ESP_LOGI("panel_web", "user %u turned user %u's browser sign-in %s", (unsigned)s->user,
             (unsigned)user, on ? "on" : "off");
    return okay(r);
}
/* A flag of the partition command: absent is false, anything but true or false a bad body. */
static bool flag(const cJSON *o, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    *out = cJSON_IsTrue(v);
    return v == NULL || cJSON_IsBool(v);
}
/* POST partition (WEB-024): arm, disarm, clear the alarm memory, acknowledge, the chime and
 * bypass as the signed-in person, without a code - their sign-in proved who they are. The engine
 * applies the same rules as their code at a keypad (alarm_engine_command_as, CORE-USR-027).
 * Each action runs only in the state it is for, checked on the alarm task in the same call that
 * runs it (alarm_service_command_as's `when`): a partition a keypad, MQTT or a keyfob armed a
 * moment ago is never disarmed by "Clear alarm", and "Arm" never changes the level of an armed
 * one. While installer mode is open only disarm and acknowledge run. */
static esp_err_t partition_action(httpd_req_t *r, session_t *s, alarm_authority_t authority,
                                  const cJSON *o)
{
    static const struct {
        const char *name;
        alarm_cmd_type_t type;
        alarm_svc_when_t when;
        alarm_walk_op_t walk_op; /* ALARM_CMD_WALK_TEST's operation */
    } ACTIONS[] = {
        {"arm", ALARM_CMD_ARM, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_TOGGLE},
        {"disarm", ALARM_CMD_DISARM, ALARM_SVC_WHEN_NOT_DISARMED, ALARM_WALK_TOGGLE},
        /* code + OFF while disarmed: clears the alarm memory, never a disarm */
        {"clear", ALARM_CMD_DISARM, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_TOGGLE},
        /* acknowledges trouble beeps and silences fire, CO and auxiliary sirens */
        {"acknowledge", ALARM_CMD_SILENCE, ALARM_SVC_WHEN_ANY, ALARM_WALK_TOGGLE},
        {"chime", ALARM_CMD_CHIME, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_TOGGLE},
        {"bypass", ALARM_CMD_BYPASS, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_TOGGLE},
        {"unbypass", ALARM_CMD_UNBYPASS, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_TOGGLE},
        /* The walk test (docs/features/40 WALK-002): start, end and extend, never the toggle, so
         * a retried request never undoes itself. A walk test runs only while the partition is
         * disarmed (installer mode can't open during one), and the engine runs each operation
         * only in the state it is for (a start while one runs, an end or extend while none:
         * bad_state). Who may: the engine's can_test() starts and extends, whoever may disarm
         * the partition ends (WALK-001, WALK-025). */
        {"walk_test_start", ALARM_CMD_WALK_TEST, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_START},
        {"walk_test_end", ALARM_CMD_WALK_TEST, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_END},
        {"walk_test_extend", ALARM_CMD_WALK_TEST, ALARM_SVC_WHEN_DISARMED, ALARM_WALK_EXTEND},
    };
    static const alarm_arm_level_t LEVELS[] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY, ALARM_LEVEL_NIGHT};
    /* Arming and disarming without a code is a person's, proved by their own password: never an
     * enrolled workstation's grant, which signs in as the master for the developer tools and
     * would otherwise disarm the house with no code anywhere (BL-113). */
    if (s->grant)
        return error(r, "403 Forbidden", "password_sign_in_required");
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd)); /* no code: cmd.code stays empty */
    const char *op = str(o, "action");
    size_t a = 0;
    while (a < sizeof(ACTIONS) / sizeof(ACTIONS[0]) && strcmp(op, ACTIONS[a].name))
        a++;
    uint32_t part, zone = 0;
    bool no_entry, silent, bypass_open, life24;
    if (a == sizeof(ACTIONS) / sizeof(ACTIONS[0]) || !number(o, "number", 0, UINT8_MAX, &part) ||
        !flag(o, "no_entry_delay", &no_entry) || !flag(o, "silent_exit", &silent) ||
        !flag(o, "bypass_open", &bypass_open) || !flag(o, "life24", &life24))
        return error(r, "400 Bad Request", "bad_request");
    /* The installer doesn't clear alarms here: the people who use the alarm do (BL-187; the
     * Overview doesn't offer it). Code + OFF at the panel still clears, as it silences. */
    if (!strcmp(op, "clear") && s->user == ALARM_USER_INSTALLER)
        return result(r, ALARM_ERR_NOT_AUTHORIZED);
    cmd.type = ACTIONS[a].type;
    if (cmd.type == ALARM_CMD_WALK_TEST) {
        cmd.walk_op = (uint8_t)ACTIONS[a].walk_op;
        /* "Include panic and 24-hour zones" (WALK-007): read by a start only */
        cmd.walk_flags =
            (uint8_t)(ACTIONS[a].walk_op == ALARM_WALK_START && life24 ? ALARM_WALK_F_LIFE24 : 0u);
    }
    if (cmd.type == ALARM_CMD_ARM) {
        const char *level = str(o, "level");
        size_t l = 0;
        while (l < sizeof(LEVELS) / sizeof(LEVELS[0]) && strcmp(level, alarm_level_name(LEVELS[l])))
            l++;
        if (l == sizeof(LEVELS) / sizeof(LEVELS[0]))
            return error(r, "400 Bad Request", "bad_request");
        cmd.level = LEVELS[l];
        /* Silent exit is Away's alone: the engine drops it for the other levels. */
        cmd.arm_flags = (uint8_t)((no_entry ? ALARM_ARM_NO_ENTRY_DELAY : 0u) |
                                  (silent ? ALARM_ARM_SILENT_EXIT : 0u) |
                                  (bypass_open ? ALARM_ARM_BYPASS_FAULTED : 0u));
    }
    if ((cmd.type == ALARM_CMD_BYPASS || cmd.type == ALARM_CMD_UNBYPASS) &&
        !number(o, "zone", 1, ALARM_MAX_ZONES, &zone))
        return error(r, "400 Bad Request", "bad_request");
    cmd.zone = (uint16_t)zone;
    /* Only the partitions this person sees, and only those in use (as the status lists them). */
    /* Only the partitions this person sees, and only those in use (as the status lists them).
     * The snapshot answers only which partitions are in use (the configuration's, not a state
     * that changes under us); the state is checked on the alarm task. */
    uint8_t viewer;
    alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
    if (!snap || !viewer_partitions(s->user, authority, &viewer)) {
        free(snap);
        return error(r, "503 Service Unavailable", "Memory busy");
    }
    alarm_service_get_snapshot(snap);
    const bool mine = part >= 1 && part <= ALARM_MAX_PARTITIONS &&
                      (snap->partitions & viewer & (1u << (part - 1))) != 0;
    free(snap);
    if (!mine)
        return error(r, "403 Forbidden", "not_allowed");
    cmd.partition = (uint8_t)part;
    alarm_cmd_result_t res = {0};
    const alarm_result_t e =
        alarm_service_command_as(&cmd, &SOURCE, s->user, ACTIONS[a].when, &res);
    ESP_LOGI("panel_web", "user %u: partition %u %s: %s", (unsigned)s->user, (unsigned)part, op,
             alarm_result_name(e));
    if (e == ALARM_OK)
        return okay(r);
    httpd_resp_set_status(r, "409 Conflict");
    cJSON *answer = cJSON_CreateObject();
    cJSON_AddStringToObject(answer, "error", alarm_result_name(e));
    /* The first zone that stopped an arm, else 0: never an echo of the request. */
    cJSON_AddNumberToObject(answer, "zone", res.not_ready_zone);
    return json(r, answer);
}
static esp_err_t action(httpd_req_t *r, session_t *s, alarm_authority_t authority, const char *path,
                        cJSON *o)
{
    char why[160] = {0};
    if (!strcmp(path, "logout")) {
        if (controller == s)
            release_control();
        mbedtls_platform_zeroize(s, sizeof(*s));
        httpd_resp_set_hdr(r, "Set-Cookie",
                           plain_http(r)
                               ? "panel_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"
                               : "panel_session=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict");
        return okay(r);
    }
    if (!strcmp(path, "password")) {
        /* Your own password: the current one first, then the new one (web_login.h). */
        if (!web_gate_open(&s_login_gate))
            return error(r, "429 Too Many Requests", "Too many wrong passwords; wait before trying again");
        bool wrong = false;
        uint8_t changed[32];
        if (!web_login_change(s->user, str(o, "current"), str(o, "password"), &wrong, changed,
                              why, sizeof(why))) {
            if (wrong)
                web_gate_failed(&s_login_gate);
            /* Flash or memory, not the person: 503, as a refused turn-off. Decided before
             * said() translates why. */
            const char *status =
                web_login_why_busy(why) ? "503 Service Unavailable" : "400 Bad Request";
            return error(r, status, said(r, why, sizeof(why)));
        }
        /* This session goes on with the new password - the one it just stored, not whatever
         * the touchscreen did since; the user's others are signed out. */
        alarm_authority_t a;
        if (!user_proof(s->user, s->proof, &a, changed))
            mbedtls_platform_zeroize(s, sizeof(*s));
        mbedtls_platform_zeroize(changed, sizeof(changed));
        ESP_LOGI("panel_web", "user %u changed their Panel Manager password", (unsigned)s->user);
        return okay(r);
    }
    if (must_change(s))
        return error(r, "403 Forbidden", "password_change_required");
    if (!allowed(authority, post_access(path, o)))
        return error(r, "403 Forbidden", "not_allowed");
    if (!locked_exempt(path, o) && panel_locked())
        return error(r, "409 Conflict", "locked");
    if (!strcmp(path, "languages"))
        return languages_post(r, o);
    if (!strcmp(path, "partition"))
        return partition_action(r, s, authority, o);
    if (!strcmp(path, "control")) {
        if (strcmp(str(o, "boot"), boot))
            return error(r, "409 Conflict", "Panel restarted; reconnect");
        const char *op = str(o, "action");
        if (controller && esp_timer_get_time() > control_until)
            release_control();
        if (!strcmp(op, "release")) {
            if (controller == s)
                release_control();
            return okay(r);
        }
        if (!strcmp(op, "acquire")) {
            if ((controller && controller != s) || !A.control || !A.control(true))
                return error(r, "409 Conflict", "The local panel or another session is in control");
            controller = s;
            pointer_sequence = 0;
            control_until = esp_timer_get_time() + 15000000;
            return okay(r);
        }
        if (!strcmp(op, "renew")) {
            if (controller != s || !A.renew || !A.renew()) {
                if (controller == s)
                    release_control();
                return error(r, "409 Conflict", "Local control took priority or lease expired");
            }
            control_until = esp_timer_get_time() + 15000000;
            return okay(r);
        }
        if (strcmp(op, "input"))
            return error(r, "400 Bad Request", "Unknown control action");
        uint32_t seq, x, y;
        const cJSON *pressed = cJSON_GetObjectItemCaseSensitive(o, "pressed");
        if (controller != s || !number(o, "sequence", 1, UINT32_MAX, &seq) ||
            seq <= pointer_sequence || !number(o, "x", 0, A.width - 1, &x) ||
            !number(o, "y", 0, A.height - 1, &y) || !cJSON_IsBool(pressed))
            return error(r, "409 Conflict", "Acquire control again");
        pointer_sequence = seq;
        if (!A.input || !A.input(seq, (uint16_t)x, (uint16_t)y, cJSON_IsTrue(pressed))) {
            release_control();
            return error(r, "409 Conflict", "Local control took priority or private screen");
        }
        control_until = esp_timer_get_time() + 2000000;
        return okay(r);
    }
    if (!strncmp(path, "config/", 7)) {
        bool ok = web_config_set(&B, path + 7, cJSON_GetObjectItemCaseSensitive(o, "values"),
                                 str(o, "operation"), why, sizeof(why));
        if (ok && !strcmp(path + 7, "display") && A.preferences_changed)
            A.preferences_changed();
        return ok ? okay(r) : error(r, "400 Bad Request", why);
    }
    if (!strcmp(path, "zigbee")) { /* the installer's, not while armed (devices.md §14) */
        /* A network reset can't be undone: the installer's password again (recent_password()),
         * never a keypad code (the touchscreen asks the installer code instead). */
        if (!strcmp(str(o, "op"), "reset")) {
            esp_err_t refused = ESP_OK;
            if (!recent_password(r, s, str(o, "me_password"), &refused))
                return refused;
        }
        const char *w = web_zigbee_post(&B, o, s->user);
        if (!w)
            return error(r, "404 Not Found", "Zigbee is not running on this panel");
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "result", w);
        return json(r, res);
    }
    if (!strcmp(path, "automations")) { /* ACC_SYSTEM, not while armed (docs/features/41) */
        /* Saving makes this person the maker an alarm or less secure action runs as (AUT-011,
         * AUT-015): a person's, proved by their own password, never an enrolled workstation's
         * grant (as for arming, BL-113). */
        if (s->grant)
            return error(r, "403 Forbidden", "password_sign_in_required");
        if (!web_automations_available())
            return error(r, "404 Not Found", "Automations are not in this firmware");
        int http = 200;
        cJSON *res = web_automations_post(o, s->user, &http);
        if (!res)
            return error(r, "503 Service Unavailable", "Memory busy");
        httpd_resp_set_status(r, http == 400   ? "400 Bad Request"
                                 : http == 409 ? "409 Conflict"
                                 : http == 503 ? "503 Service Unavailable"
                                               : HTTPD_200);
        return json(r, res);
    }
    if (!strcmp(path, "zwave")) { /* the installer's, not while armed (screens.md §11) */
        const char *w = web_zwave_post(&B, o, s->user);
        if (!w)
            return error(r, "404 Not Found", "Z-Wave is not running on this panel");
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "result", w);
        return json(r, res);
    }
    if (!strcmp(path, "remote")) {
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "web"),
                    *d = cJSON_GetObjectItemCaseSensitive(o, "developer");
        if (!cJSON_IsBool(w) || !cJSON_IsBool(d))
            return error(r, "400 Bad Request", "Both toggles are required");
        /* Each lets a computer in: your password again (recent_password()). */
        esp_err_t refused = ESP_OK;
        if (!recent_password(r, s, str(o, "me_password"), &refused))
            return refused;
        return panel_web_set(cJSON_IsTrue(w), cJSON_IsTrue(d)) == ESP_OK
                   ? okay(r)
                   : error(r, "409 Conflict", "Could not persist remote access");
    }
    if (!strcmp(path, "keypads")) {
        if (s->grant) return error(r, "403 Forbidden", "password_sign_in_required");
        esp_err_t refused = ESP_OK;
        if (!recent_password(r, s, str(o, "me_password"), &refused)) return refused;
        const char *why_keypad = web_keypads_post(s->user, o);
        if (!why_keypad && !strcmp(str(o, "op"), "save") &&
            cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "device")) == 0 && A.preferences_changed)
            A.preferences_changed();
        return why_keypad ? error(r, "409 Conflict", why_keypad) : okay(r);
    }
    if (!strcmp(path, "alarm")) {
        /* Programming as the signed-in installer, without a code (CORE-PROG-011): the engine
         * enters installer mode as this user, read again on the alarm task. */
        if (authority != ALARM_AUTH_INSTALLER)
            return error(r, "403 Forbidden", "not_allowed");
        alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
        alarm_config_t *cfg = web_alloc(sizeof(*cfg));
        if (!snap || !cfg) {
            free(snap);
            free(cfg);
            return error(r, "503 Service Unavailable", "Memory busy");
        }
        alarm_service_get_snapshot(snap);
        uint32_t revision;
        bool current = number(o, "revision", 0, UINT32_MAX, &revision) &&
                       revision == snap->config_seq && !snap->program;
        free(snap);
        if (!current) {
            free(cfg);
            return error(r, "409 Conflict",
                         "Configuration changed or installer mode is already occupied; reload");
        }
        if (alarm_service_get_config(cfg) != ESP_OK ||
            !web_alarm_config_patch(cfg, cJSON_GetObjectItemCaseSensitive(o, "changes"), why,
                                    sizeof(why))) {
            mbedtls_platform_zeroize(cfg, sizeof(*cfg)); /* it holds the code hashes */
            free(cfg);
            return error(r, "400 Bad Request", why[0] ? why : "Invalid configuration");
        }
        if (!web_keypads_config_valid(cJSON_GetObjectItemCaseSensitive(o, "changes"), cfg)) {
            mbedtls_platform_zeroize(cfg, sizeof(*cfg)); free(cfg);
            return error(r, "409 Conflict", "Move assigned keypads first; edit paired keypads in their popup");
        }
        const alarm_result_t e = alarm_service_program_apply_as(&SOURCE, s->user, revision, cfg);
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
        return result(r, e);
    }
    if (!strcmp(path, "people")) {
        /* As the signed-in person, without a code (CORE-USR-028): the engine applies their own
         * authority, read again at every change, as their code at a keypad. A person's, proved
         * by their own password: never an enrolled workstation's grant, which signs in as the
         * master for the developer tools (as for arming, BL-113). */
        if (s->grant)
            return error(r, "403 Forbidden", "password_sign_in_required");
        uint32_t user, auth, parts;
        if (!number(o, "number", 1, ALARM_MAX_USERS, &user))
            return error(r, "400 Bad Request", "bad_request");
        const char *op = str(o, "action");
        /* Your own password changes on Your password, which asks for the current one (POST
         * password), and turning your own sign-in off stays at the panel: here, a session left
         * open could choose a password its owner doesn't know and pass every guarded change. */
        if ((!strcmp(op, "web-password") || !strcmp(op, "web-off")) && user == s->user)
            return error(r, "403 Forbidden", "not_allowed");
        if (!strcmp(op, "web-password")) {
            /* temporary: missing is true (a page from before it asked); anything else a bool */
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "temporary");
            if (t && !cJSON_IsBool(t))
                return error(r, "400 Bad Request", "bad_request");
            return people_login(r, s, (uint8_t)user, true, str(o, "password"),
                                !t || cJSON_IsTrue(t));
        }
        if (!strcmp(op, "web-off"))
            return people_login(r, s, (uint8_t)user, false, "", false);
        alarm_result_t e;
        if (!strcmp(op, "delete"))
            e = alarm_service_user_delete_as(&SOURCE, s->user, (uint8_t)user);
        else if (number(o, "authority", 0, ALARM_AUTH_DURESS, &auth) &&
                 number(o, "partitions", 0, 255, &parts)) {
            if (str(o, "new_code")[0])
                e = alarm_service_user_set_as(&SOURCE, s->user, (uint8_t)user,
                                              str(o, "new_code"), (alarm_authority_t)auth,
                                              (uint8_t)parts, str(o, "name"));
            else
                e = alarm_service_user_update_as(&SOURCE, s->user, (uint8_t)user,
                                                 (alarm_authority_t)auth, (uint8_t)parts,
                                                 str(o, "name"));
        } else
            e = ALARM_ERR_BAD_ARG;
        return result(r, e);
    }
    if (!strcmp(path, "time")) {
        /* The date and time by hand (docs/features/35 RTC-008): only with a clock module, which
         * B.clock_set checks; UTC milliseconds, so the browser's own time zone doesn't matter. */
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(o, "epoch_ms");
        if (ms) {
            if (!B.clock_set || !cJSON_IsNumber(ms) || !isfinite(ms->valuedouble) ||
                ms->valuedouble < 0 || ms->valuedouble > 9e15)
                return error(r, "400 Bad Request", "Invalid time");
            char why[64] = "";
            return B.clock_set(B.ctx, (int64_t)ms->valuedouble, s->user, why, sizeof(why))
                       ? okay(r)
                       : error(r, "409 Conflict", why[0] ? said(r, why, sizeof(why)) : "The clock was not set");
        }
        return net_wifi_set_timezone(str(o, "timezone")) == ESP_OK &&
                       net_wifi_set_ntp_server(str(o, "ntp")) == ESP_OK
                   ? okay(r)
                   : error(r, "400 Bad Request", "Invalid time settings");
    }
    if (!strcmp(path, "wifi")) {
        if (!str(o, "ssid")[0] || strlen(str(o, "ssid")) > 32 || strlen(str(o, "password")) > 64)
            return error(r, "400 Bad Request", "Invalid network credentials");
        return net_wifi_trial_credentials(str(o, "ssid"), str(o, "password"),
                                          str(o, "operation")) == ESP_OK
                   ? okay(r)
                   : error(r, "409 Conflict", "Could not start Wi-Fi connection");
    }
    if (!strcmp(path, "home_assistant") && B.halink_status) {
        const char *op = str(o, "action");
        if (!strcmp(op, "enable"))
            B.halink_enable(B.ctx, true);
        else if (!strcmp(op, "disable"))
            B.halink_enable(B.ctx, false);
        else if (!strcmp(op, "pair"))
            B.halink_pair_begin(B.ctx);
        else if (!strcmp(op, "accept"))
            B.halink_pair_decide(B.ctx, true);
        else if (!strcmp(op, "reject"))
            B.halink_pair_decide(B.ctx, false);
        else if (!strcmp(op, "cancel"))
            B.halink_pair_cancel(B.ctx);
        else if (!strcmp(op, "unpair"))
            B.halink_unpair(B.ctx);
        else
            return error(r, "400 Bad Request", "Unknown pairing action");
        return okay(r);
    }
    if (!strcmp(path, "updates")) { /* post_access, locked_exempt: OTA-022, OTA-027, OTA-030 */
        const char *op = str(o, "action");
        if (!strcmp(op, "check")) {
            const int going = ota_running_now();
            if (going < 0)
                return error(r, "503 Service Unavailable", "Memory busy");
            if (going)
                return error(r, "409 Conflict", "busy");
            const esp_err_t e = panel_ota_check();
            return e == ESP_OK ? okay(r) : error(r, "409 Conflict", esp_err_to_name(e));
        }
        if (!strcmp(op, "install")) {
            const esp_err_t e = panel_ota_install(&SOURCE, s->user, false);
            return e == ESP_OK                  ? okay(r)
                   : e == ESP_ERR_INVALID_STATE ? error(r, "409 Conflict", "busy")
                                                : error(r, "409 Conflict", esp_err_to_name(e));
        }
        if (!strcmp(op, "cancel")) { /* Stop (OTA-027): allowed while armed */
            const esp_err_t e = panel_ota_stop(&SOURCE, s->user);
            return e == ESP_OK                  ? okay(r)
                   : e == ESP_ERR_NOT_FOUND     ? error(r, "409 Conflict", "no_update")
                   : e == ESP_ERR_NOT_SUPPORTED ? error(r, "409 Conflict", "not_stoppable")
                                                : error(r, "409 Conflict", esp_err_to_name(e));
        }
        if (!strcmp(op, "server_check")) { /* answers at once; GET updates' check follows it */
            const esp_err_t e = panel_ota_server_check(str(o, "domain"));
            return e == ESP_OK                  ? okay(r)
                   : e == ESP_ERR_INVALID_ARG   ? error(r, "400 Bad Request", "bad_domain")
                   : e == ESP_ERR_INVALID_STATE ? error(r, "409 Conflict", "update_running")
                                                : error(r, "409 Conflict", esp_err_to_name(e));
        }
        if (!strcmp(op, "server")) {
            /* A new server is trusted to replace the firmware: a key the panel doesn't trust yet
             * needs the installer's password again (recent_password()); one it trusts already
             * (going back to roboalarms.com) only the review. Only the key the check showed is
             * kept (OTA-020, OTA-021). */
            uint8_t digest[32];
            if (!ota_digest(str(o, "key_sha256"), digest))
                return error(r, "400 Bad Request", "bad_request");
            panel_ota_srv_check_t *chk = malloc(sizeof(*chk));
            if (chk == NULL)
                return error(r, "503 Service Unavailable", "Memory busy");
            panel_ota_server_check_get(chk);
            const bool trusted = chk->state == PANEL_OTA_SRV_DONE &&
                                 chk->result == PANEL_OTA_SRV_OK && chk->trusted &&
                                 !memcmp(chk->key_sha256, digest, sizeof(digest)) &&
                                 !strcmp(chk->domain, str(o, "domain"));
            free(chk);
            esp_err_t refused = ESP_OK;
            if (!trusted && !recent_password(r, s, str(o, "me_password"), &refused))
                return refused;
            const int going = ota_running_now();
            const int factory = going == 0 ? ota_factory_code() : 0;
            if (going < 0 || factory < 0)
                return error(r, "503 Service Unavailable", "Memory busy");
            if (going)
                return error(r, "409 Conflict", "update_running");
            const esp_err_t e = panel_ota_server_commit(str(o, "domain"), digest, factory == 1,
                                                        &SOURCE, s->user);
            return e == ESP_OK                  ? okay(r)
                   : e == ESP_ERR_NOT_ALLOWED   ? error(r, "409 Conflict", "factory_code")
                   : e == ESP_ERR_INVALID_STATE ? error(r, "409 Conflict", "stale")
                                                : error(r, "409 Conflict", esp_err_to_name(e));
        }
        if (!strcmp(op, "channel")) { /* OTA-019 */
            const esp_err_t e = panel_ota_set_channel(str(o, "channel"), &SOURCE, s->user);
            return e == ESP_OK                  ? okay(r)
                   : e == ESP_ERR_INVALID_ARG   ? error(r, "400 Bad Request", "bad_request")
                   : e == ESP_ERR_INVALID_STATE ? error(r, "409 Conflict", "update_running")
                                                : error(r, "409 Conflict", esp_err_to_name(e));
        }
        return error(r, "400 Bad Request", "Unknown update action");
    }
    if (!strcmp(path, "upload-install")) {
        if (!atomic_load(&dev_on) || s->user != staged_user || !staged_operation[0] ||
            strcmp(str(o, "operation"), staged_operation))
            return error(r, "403 Forbidden", "No matching signed upload");
        const char *reason = NULL;
        if (!panel_ota_system_idle(&reason))
            return error(r, "409 Conflict", "Panel must be disarmed and idle");
        esp_err_t e = panel_ota_remote_prepare(staged_operation, staged_elf);
        if (e == ESP_OK)
            e = panel_ota_apply(&SOURCE, s->user);
        return e == ESP_OK ? okay(r) : error(r, "409 Conflict", esp_err_to_name(e));
    }
    if (!strcmp(path, "developer/enroll")) {
        if (!atomic_load(&dev_on))
            return error(r, "403 Forbidden", "Developer mode is disabled");
        /* The certificate enrolled here signs in as the master and updates the firmware,
         * for as long as it exists: only the installer, who alone can switch developer mode
         * on, gets to choose it (a master's own deletion would not revoke it), and with their
         * password again (recent_password()). */
        if (authority != ALARM_AUTH_INSTALLER)
            return error(r, "403 Forbidden", "Installer authority required");
        esp_err_t refused = ESP_OK;
        if (!recent_password(r, s, str(o, "me_password"), &refused))
            return refused;
        const char *pem = str(o, "certificate");
        esp_err_t e = dev_remote_enroll_public((const uint8_t *)pem, strlen(pem) + 1);
        if (e != ESP_OK)
            return error(r, "400 Bad Request", "A valid public client certificate is required");
        char *der = web_alloc(2049);
        if (!der)
            return error(r, "500 Internal Server Error", "Memory busy");
        dev_remote_public_identity(der, 2049);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "id", identity);
        cJSON_AddStringToObject(result, "host", str(o, "host"));
        cJSON_AddNumberToObject(result, "port", 6055);
        cJSON_AddStringToObject(result, "certificate_der", der);
        free(der);
        return json(r, result);
    }
    /* Factory reset can't be undone: your password again (web_extra.c checks the rest). */
    if (!strcmp(path, "factory-reset") && authority == ALARM_AUTH_INSTALLER) {
        esp_err_t refused = ESP_OK;
        if (!recent_password(r, s, str(o, "me_password"), &refused))
            return refused;
    }
    cJSON *extra = web_extra_action(&B, path, o, s->user, authority, request_words(r));
    if (extra) {
        if (memory_busy(extra))
            httpd_resp_set_status(r, "503 Service Unavailable");
        else if (cJSON_HasObjectItem(extra, "error"))
            httpd_resp_set_status(r, "409 Conflict");
        return json(r, extra);
    }
    return error(r, "404 Not Found", "Not included in this firmware");
}
/* POST /api/v1/setup: setting up a headless panel, signed in or not (web_setup.c decides). */
static esp_err_t setup_post(httpd_req_t *r, session_t *s, alarm_authority_t authority)
{
    if (!origin_ok(r))
        return error(r, "403 Forbidden", "Same-origin HTTPS required");
    cJSON *o = body(r);
    if (!o)
        return error(r, "400 Bad Request", "Expected bounded JSON body");
    const char *status = "404 Not Found", *why = "not_found";
    cJSON *out = web_setup_action(o, s ? s->user : 0, authority, via_setup_wifi(r), panel_locked(),
                                  &status, &why);
    erase_json(o);
    cJSON_Delete(o);
    if (!out)
        return error(r, status, why);
    return json(r, out);
}
static esp_err_t post(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    alarm_authority_t authority = ALARM_AUTH_NONE;
    session_t *s = session(r, &authority);
    char setup_path[96];
    api_path(r, setup_path, sizeof(setup_path));
    if (!strcmp(setup_path, "setup"))
        return setup_post(r, s, authority);
    if (web_setup_unclaimed())
        return error(r, "403 Forbidden", "setup_first");
    if (!s)
        return error(r, "401 Unauthorized", "Sign in again or reload the page");
    cJSON *o = body(r);
    if (!o)
        return error(r, "400 Bad Request", "Expected bounded JSON body");
    char path[96];
    api_path(r, path, sizeof(path));
    esp_err_t e = action(r, s, authority, path, o);
    erase_json(o);
    cJSON_Delete(o);
    return e;
}

static esp_err_t continuity(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    /* This random token can only finish the operation already authorized before a
     * reboot/network change. It cannot authenticate a browser or start a change. */
    if (!atomic_load(&web_on) || !origin_ok(r))
        return error(r, "403 Forbidden", "Same-origin HTTPS required");
    cJSON *o = body(r);
    if (!o)
        return error(r, "400 Bad Request", "Expected JSON");
    const char *token = str(o, "operation");
    bool ok = false;
    if (strlen(token) == 32 && strspn(token, "0123456789abcdef") == 32) {
        char path[96];
        api_path(r, path, sizeof(path));
        if (!strcmp(path, "network-confirm"))
            ok = net_wifi_trial_confirm(token);
        else {
            char current[33], elf[65];
            bool required;
            panel_ota_remote_operation(current, &required);
            hex(esp_app_get_description()->app_elf_sha256, 32, elf);
            if (strlen(current) == 32 && equal(current, token, 32) && !strcmp(str(o, "elf"), elf)) {
                panel_ota_status_t st;
                panel_ota_get_status(&st);
                ok = !st.probation || panel_ota_remote_confirm(token);
                if (ok) {
                    erase_json(o);
                    cJSON_Delete(o);
                    o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", true);
                    cJSON_AddBoolToObject(o, "complete", !st.probation);
                    return json(r, o);
                }
            }
        }
    }
    erase_json(o);
    cJSON_Delete(o);
    return ok ? okay(r) : error(r, "409 Conflict", "Operation is not ready or no longer pending");
}
/* ---- the backup file (docs/features/14 CFG-005, CFG-006) --------------------------------------
 * POST /api/v1/backup {"password", "me_password"}: the file, encrypted with that password, as a
 * download. PUT /api/v1/backup: the body is a file; X-Backup-Password (the password's bytes in
 * hex, so spaces and punctuation survive a header) and X-Panel-Password (the signed-in
 * person's own password, the same way). Both need the installer signed in and a recent
 * password (recent_password()); a restore needs every partition disarmed and quiet, and the
 * panel restarts two seconds after answering. */
static bool unhex(const char *hex, char *out, size_t cap)
{
    const size_t n = strlen(hex);
    if (n % 2 || n / 2 >= cap)
        return false;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]) ||
            sscanf(hex + 2 * i, "%2x", &v) != 1)
            return false;
        out[i] = (char)v;
    }
    out[n / 2] = 0;
    return true;
}
static esp_err_t backup_post(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    if (!s)
        return error(r, "401 Unauthorized", "Sign in again or reload the page");
    if (must_change(s) || authority != ALARM_AUTH_INSTALLER)
        return error(r, "403 Forbidden", "not_allowed");
    cJSON *o = body(r);
    if (!o)
        return error(r, "400 Bad Request", "Expected bounded JSON body");
    esp_err_t refused = ESP_OK;
    if (!recent_password(r, s, str(o, "me_password"), &refused)) {
        erase_json(o);
        cJSON_Delete(o);
        return refused;
    }
    uint8_t *file = NULL;
    size_t len = 0;
    backup_result_t why = BACKUP_OK;
    const esp_err_t e = backup_service_make(str(o, "password"), &SOURCE, s->user, &file, &len, &why);
    erase_json(o);
    cJSON_Delete(o);
    if (e != ESP_OK)
        return why == BACKUP_ERR_PASSWORD
                   ? error(r, "400 Bad Request", "backup_password")
                   : error(r, "503 Service Unavailable", "backup_failed");
    char name[64] = "panel-backup.rabk";
    const time_t now = time(NULL);
    struct tm tm;
    if (now > 1735689600 && localtime_r(&now, &tm))
        strftime(name, sizeof(name), "panel-backup-%Y-%m-%d.rabk", &tm);
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    headers(r);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", disposition);
    const esp_err_t sent = httpd_resp_send(r, (const char *)file, (ssize_t)len);
    mbedtls_platform_zeroize(file, len);
    free(file);
    return sent;
}
static esp_err_t backup_put(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    if (!s)
        return error(r, "401 Unauthorized", "Sign in again or reload the page");
    if (must_change(s) || authority != ALARM_AUTH_INSTALLER)
        return error(r, "403 Forbidden", "not_allowed");
    if (panel_locked())
        return error(r, "409 Conflict", "locked");
    if (r->content_len < BACKUP_OVERHEAD || r->content_len > BACKUP_PAYLOAD_MAX + BACKUP_OVERHEAD)
        return error(r, "400 Bad Request", "backup_format");
    char hex[2 * BACKUP_PASSWORD_MAX + 2] = {0}, password[BACKUP_PASSWORD_MAX + 1];
    {
        char me_hex[2 * WEB_PASSWORD_MAX + 2] = {0}, me[WEB_PASSWORD_MAX + 1] = {0};
        if (httpd_req_get_hdr_value_str(r, "X-Panel-Password", me_hex, sizeof(me_hex)) != ESP_OK ||
            !unhex(me_hex, me, sizeof(me)))
            me[0] = 0; /* missing or malformed: as none */
        esp_err_t refused = ESP_OK;
        const bool recent = recent_password(r, s, me, &refused);
        mbedtls_platform_zeroize(me_hex, sizeof(me_hex));
        mbedtls_platform_zeroize(me, sizeof(me));
        if (!recent)
            return refused;
    }
    if (httpd_req_get_hdr_value_str(r, "X-Backup-Password", hex, sizeof(hex)) != ESP_OK ||
        !unhex(hex, password, sizeof(password))) {
        mbedtls_platform_zeroize(hex, sizeof(hex));
        return error(r, "400 Bad Request", "backup_decrypt");
    }
    mbedtls_platform_zeroize(hex, sizeof(hex));
    uint8_t *file = heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM);
    if (!file) {
        mbedtls_platform_zeroize(password, sizeof(password));
        return error(r, "503 Service Unavailable", "Memory busy");
    }
    size_t got = 0;
    const int64_t deadline = esp_timer_get_time() + 60000000;
    while (got < r->content_len && esp_timer_get_time() < deadline) {
        const int n = httpd_req_recv(r, (char *)file + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    if (got == r->content_len)
        s_body_unread = false;
    backup_summary_t sum;
    memset(&sum, 0, sizeof(sum));
    backup_result_t why = BACKUP_ERR_FORMAT;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (got == r->content_len)
        e = backup_service_restore(file, got, password, &SOURCE, s->user, &sum, &why);
    mbedtls_platform_zeroize(password, sizeof(password));
    heap_caps_free(file);
    if (e == ESP_ERR_INVALID_STATE)
        return error(r, "409 Conflict", "locked");
    if (e == ESP_ERR_INVALID_ARG) {
        static const char *const words[] = {
            [BACKUP_OK] = "backup_format", [BACKUP_ERR_ARG] = "backup_format",
            [BACKUP_ERR_PASSWORD] = "backup_decrypt", [BACKUP_ERR_FORMAT] = "backup_format",
            [BACKUP_ERR_DECRYPT] = "backup_decrypt", [BACKUP_ERR_CRYPTO] = "backup_failed",
            [BACKUP_ERR_BOARD] = "backup_board", [BACKUP_ERR_CONTENT] = "backup_content"};
        return error(r, "400 Bad Request",
                     (unsigned)why < sizeof(words) / sizeof(words[0]) ? words[why] : "backup_format");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", e == ESP_OK);
    cJSON_AddBoolToObject(o, "restarting", true);
    cJSON_AddNumberToObject(o, "zones", sum.zones);
    cJSON_AddNumberToObject(o, "users", sum.users);
    cJSON_AddStringToObject(o, "firmware", sum.meta.firmware);
    cJSON_AddNumberToObject(o, "created", (double)sum.meta.created_s);
    cJSON_AddBoolToObject(o, "network", sum.network);
    cJSON_AddBoolToObject(o, "zwave", sum.zwave);
    if (e != ESP_OK)
        cJSON_AddStringToObject(o, "error", "backup_partial");
    return json(r, o);
}
/* PUT /api/v1/zwave-radio (docs/features/36 network.md §14, screens.md §11): the installer's own
 * .gbl for the Z-Wave radio - the body is the file. Installer signed in; the Z-Wave service
 * checks the file, that every partition is disarmed, and holds arming while it runs. */
static esp_err_t zwave_radio_put(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    if (!s || must_change(s) || authority != ALARM_AUTH_INSTALLER)
        return error(r, "403 Forbidden", "The installer signed in is required");
    if (r->content_len < 16 || r->content_len > 1024 * 1024)
        return error(r, "400 Bad Request", "Not a Z-Wave radio file");
    uint8_t *file = heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM);
    if (!file)
        return error(r, "503 Service Unavailable", "Memory busy");
    size_t got = 0;
    const int64_t deadline = esp_timer_get_time() + 120000000;
    while (got < r->content_len && esp_timer_get_time() < deadline) {
        const int n = httpd_req_recv(r, (char *)file + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    if (got == r->content_len)
        s_body_unread = false;
    const char *w = got == r->content_len ? web_zwave_radio_file(file, got, s->user) : "invalid";
    heap_caps_free(file);
    if (!w)
        return error(r, "404 Not Found", "Z-Wave is not running on this panel");
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "result", w);
    return json(r, res);
}

static esp_err_t upload(httpd_req_t *r)
{
    s_body_unread = r->content_len > 0;
    alarm_authority_t authority;
    session_t *s = session(r, &authority);
    if (!s || !atomic_load(&dev_on) || must_change(s) || authority != ALARM_AUTH_INSTALLER)
        return error(r, "403 Forbidden",
                     "Developer access and the installer signed in are required");
    const char *idle_reason = NULL;
    if (!panel_ota_system_idle(&idle_reason))
        return error(r, "409 Conflict", "Panel must be disarmed and idle");
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || r->content_len < 288 || r->content_len > slot->size)
        return error(r, "400 Bad Request", "Invalid image size");
    char sha_hex[65], older[8] = {0};
    uint8_t sha[32];
    if (httpd_req_get_hdr_value_str(r, "X-Image-SHA256", sha_hex, sizeof(sha_hex)) != ESP_OK ||
        strlen(sha_hex) != 64 || strspn(sha_hex, "0123456789abcdef") != 64)
        return error(r, "400 Bad Request", "Image SHA256 required");
    for (unsigned i = 0; i < 32; i++) {
        unsigned n;
        sscanf(sha_hex + i * 2, "%2x", &n);
        sha[i] = (uint8_t)n;
    }
    httpd_req_get_hdr_value_str(r, "X-Allow-Older", older, sizeof(older));
    if (!strcmp(older, "yes") && authority != ALARM_AUTH_INSTALLER)
        return error(r, "403 Forbidden", "Only the installer may install older firmware");
    esp_err_t e = panel_ota_begin((uint32_t)r->content_len, sha, !strcmp(older, "yes"));
    if (e != ESP_OK)
        return error(r, "409 Conflict", "Another update or startup probation is active");
    release_control();
    staged_operation[0] = 0;
    char why[160] = {0}, version[32];
    uint8_t *buf = malloc(4096); /* internal: flash writes need no bounce copy */
    size_t left = r->content_len;
    int64_t deadline = esp_timer_get_time() + 300000000;
    if (!buf)
        e = ESP_ERR_NO_MEM;
    while (e == ESP_OK && left) {
        if (!atomic_load(&web_on) || !atomic_load(&dev_on) || esp_timer_get_time() > deadline) {
            e = ESP_ERR_TIMEOUT;
            break;
        }
        int n = httpd_req_recv(r, (char *)buf, left < 4096 ? left : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue; /* Wi-Fi paused for a moment: the 5 minute deadline decides, not 3 s */
        if (n <= 0) {
            e = ESP_FAIL;
            break;
        }
        e = panel_ota_write(buf, (size_t)n, why, sizeof(why));
        left -= (size_t)n;
    }
    if (left == 0)
        s_body_unread = false;
    free(buf);
    if (e == ESP_OK)
        e = panel_ota_end(version, sizeof(version), why, sizeof(why));
    else
        panel_ota_abort();
    if (e != ESP_OK)
        return error(r, "400 Bad Request", why[0] ? why : esp_err_to_name(e));
    esp_app_desc_t app;
    if (esp_ota_get_partition_description(slot, &app) != ESP_OK ||
        strcmp(app.project_name, esp_app_get_description()->project_name)) {
        panel_ota_discard(); /* not left "ready" for anyone to apply */
        return error(r, "409 Conflict", "Upload must match this board and firmware profile");
    }
    uint8_t token[16];
    esp_fill_random(token, sizeof(token));
    hex(token, sizeof(token), staged_operation);
    memcpy(staged_elf, app.app_elf_sha256, 32);
    staged_user = s->user;
    char elf[65];
    hex(staged_elf, 32, elf);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "operation", staged_operation);
    cJSON_AddStringToObject(o, "elf", elf);
    cJSON_AddStringToObject(o, "version", version);
    return json(r, o);
}
void panel_web_get(bool *web, bool *developer)
{
    *web = atomic_load(&web_on);
    *developer = atomic_load(&dev_on);
}
esp_err_t panel_web_set(bool web, bool developer)
{
    if (!settings_lock)
        return ESP_ERR_INVALID_STATE;
    if (developer && !web)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t e = nvs_open("panelweb", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, "mode", (web ? 1 : 0) | (developer ? 2 : 0));
        if (e == ESP_OK)
            e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        atomic_store(&web_on, web);
        atomic_store(&dev_on, developer);
        grant_until = 0;
        mbedtls_platform_zeroize(grant_hash, sizeof(grant_hash));
        dev_remote_set_allowed(developer);
        atomic_fetch_add(&generation, 1);
    }
    xSemaphoreGive(settings_lock);
    return e;
}
static void ui_get(void *ctx, bool *web, bool *dev)
{
    (void)ctx;
    panel_web_get(web, dev);
}
static bool ui_set(void *ctx, bool web, bool dev)
{
    (void)ctx;
    return panel_web_set(web, dev) == ESP_OK;
}
static void ui_info(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    net_wifi_status_t wifi;
    net_wifi_get_status(&wifi);
    uint8_t hash[32];
    char fingerprint[65] = "unavailable";
    if (cert_len && digest(cert, cert_len, hash))
        hex(hash, 32, fingerprint);
    snprintf(out, cap, "https://%s\nCertificate SHA-256:\n%.16s %.16s %.16s %.16s", wifi.ip,
             fingerprint, fingerprint + 16, fingerprint + 32, fingerprint + 48);
}
static bool ui_login_get(void *ctx, uint8_t user)
{
    (void)ctx;
    alarm_config_t *cfg = web_alloc(sizeof(*cfg)); /* fresh: see web_login_enabled() */
    const bool on = cfg && alarm_service_get_config(cfg) == ESP_OK && web_login_enabled(cfg, user);
    if (cfg) {
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
    }
    return on;
}
static bool ui_login_set(void *ctx, uint8_t user, const char *password, bool temporary, char *why,
                         size_t len)
{
    (void)ctx;
    return web_login_set(user, password, temporary, why, len);
}
static void ui_login_clear(void *ctx, uint8_t user)
{
    (void)ctx;
    (void)web_login_clear(user); /* off at once; an erase flash refused is retried (web_login.h) */
}
static void ui_remote_pause(void *ctx, bool paused)
{
    (void)ctx;
    web_screen_pause(paused);
}
static void (*s_about)(void *ctx, ui_about_t *out);
static void ui_about(void *ctx, ui_about_t *out)
{
    s_about(ctx, out);
    snprintf(out->panel_id, sizeof(out->panel_id), "%s", web_login_panel_id());
}
void panel_web_ui_backend(ui_backend_t *b)
{
    b->remote_get = ui_get;
    b->remote_set = ui_set;
    b->remote_info = ui_info;
    b->web_login_get = ui_login_get;
    b->web_login_set = ui_login_set;
    b->web_login_clear = ui_login_clear;
    b->remote_pause = ui_remote_pause;
    if (b->about && b->about != ui_about) {
        s_about = b->about;
        b->about = ui_about;
    }
}
/* A route that can't be registered costs that page, never the panel (docs/bugs/B017: a table one
 * slot short aborted every start). tools/tests/test_panel_web_routes.py keeps the routes within
 * WEB_URI_HANDLERS. */
/* Every handler of both servers runs under s_serve_lock (taken for the call, the route's function
 * in user_ctx). */
static esp_err_t serialized(httpd_req_t *r)
{
    esp_err_t (*fn)(httpd_req_t *) = (esp_err_t (*)(httpd_req_t *))r->user_ctx;
    xSemaphoreTakeRecursive(s_serve_lock, portMAX_DELAY);
    const esp_err_t e = fn(r);
    xSemaphoreGiveRecursive(s_serve_lock);
    return e;
}
static void route(const char *uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = method, .handler = serialized, .user_ctx = (void *)fn};
    const esp_err_t e = httpd_register_uri_handler(server, &u);
    if (e != ESP_OK)
        ESP_LOGE("panel_web", "route %s not registered: %s", uri, esp_err_to_name(e));
}
static esp_err_t low_latency_socket(httpd_handle_t handle, int socket)
{
    (void)handle;
    const int enabled = 1;
    /* Small control responses must not wait for Nagle/delayed ACK batching. */
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0 ? ESP_OK
                                                                                        : ESP_FAIL;
}
/* Every socket that closes (the server calls this instead of closing it): the remote keypad
 * forgets it before its number can be given to another connection. */
static void socket_closed(httpd_handle_t handle, int socket)
{
    (void)handle;
    web_screen_closed(socket);
    close(socket);
}
/* ---- plain HTTP on port 80: straight to HTTPS (WEB-023) ------------------------------------
 * Someone who types the panel's address without "https://" reaches port 80. The recommended
 * answer is a permanent redirect to the same page over HTTPS, and nothing else: no page, no API,
 * no cookie is ever served in the clear. The address kept is the one the person typed (the Host
 * header) when it is plainly a host, else the panel's own (http_redirect.c), so the Location
 * can't be split or sent elsewhere. "no-store": on a home network an address is handed to
 * another device later, and a browser that remembered this redirect forever would keep sending
 * that device's address to HTTPS. No HSTS on the HTTPS side: the panel's certificate is its own
 * (checked by its fingerprint), and HSTS would stop a browser letting the person accept it. */
static esp_err_t redirect(httpd_req_t *r)
{
    char host[HTTP_REDIRECT_HOST_MAX + 8] = "";
    const bool have_host = httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) == ESP_OK;
    net_wifi_status_t w;
    net_wifi_get_status(&w);
    char location[CONFIG_HTTPD_MAX_URI_LEN + HTTP_REDIRECT_HOST_MAX + 24];
    if (!http_redirect_location(have_host ? host : NULL, w.ip, r->uri, 443, location,
                                sizeof(location)))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use https:// and the panel's address");
    httpd_resp_set_status(r, "301 Moved Permanently");
    httpd_resp_set_hdr(r, "Location", location);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(r, NULL, 0);
}
/* Port 80 on a board without a screen: a phone on the setup Wi-Fi gets the Panel Manager itself
 * while setup or recovery is open; anyone else, and everyone once setup is done, the redirect. */
static esp_err_t plain_serve(httpd_req_t *r)
{
    if (!via_setup_wifi(r) || !web_setup_listener())
        return redirect(r);
    /* Any other address the phone asked for (the setup Wi-Fi's name server answers every name
     * with the panel, web_setup_dns.c) goes to the setup page, as a router's setup does; so do
     * the phones' own "am I online?" checks, which then open the setup page by themselves. */
    char host[HTTP_REDIRECT_HOST_MAX + 8] = "";
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK ||
        strcmp(host, NET_WIFI_SETUP_AP_IP) != 0) {
        httpd_resp_set_status(r, "302 Found");
        httpd_resp_set_hdr(r, "Location", "http://" NET_WIFI_SETUP_AP_IP "/");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        return httpd_resp_send(r, NULL, 0);
    }
    return serialized(r);
}
static void plain_route(const char *uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = method, .handler = plain_serve, .user_ctx = (void *)fn};
    if (httpd_register_uri_handler(redirect_server, &u) != ESP_OK)
        ESP_LOGE("panel_web", "HTTP 80 route %s not registered", uri);
}
static void redirect_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = 80;
    c.ctrl_port = 32771; /* 32769 dev_remote, 32770 the HTTPS server */
    c.max_open_sockets = 2; /* a redirect is one short request; LWIP's budget is 24 */
    c.max_uri_handlers = 2;
    c.stack_size = 4096;
    if (s_setup_board) {
        /* It serves setup's pages and API too (plain_serve): the HTTPS worker's stack, a page's
         * few files at once. */
        c.max_open_sockets = 4;
        c.max_uri_handlers = 6;
        c.stack_size = 16384;
    }
    c.task_priority = 2;
    c.recv_wait_timeout = 3;
    c.send_wait_timeout = 3;
    c.lru_purge_enable = true;
    c.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t e = httpd_start(&redirect_server, &c);
    if (e != ESP_OK) {
        redirect_server = NULL;
        ESP_LOGE("panel_web", "HTTP 80 redirect not started: %s (HTTPS 443 unaffected)",
                 esp_err_to_name(e));
        return;
    }
    if (s_setup_board) {
        /* First match wins, as on HTTPS: sign-in, the API, then files (setup's pages). */
        plain_route("/api/v1/login", HTTP_POST, login);
        plain_route("/api/v1/*", HTTP_GET, get);
        plain_route("/api/v1/*", HTTP_POST, post);
    }
    const httpd_uri_t get = {.uri = "/*", .method = HTTP_GET,
                             .handler = s_setup_board ? plain_serve : redirect,
                             .user_ctx = (void *)assets};
    const httpd_uri_t head = {.uri = "/*", .method = HTTP_HEAD, .handler = redirect};
    if (httpd_register_uri_handler(redirect_server, &get) != ESP_OK ||
        httpd_register_uri_handler(redirect_server, &head) != ESP_OK) {
        httpd_stop(redirect_server);
        redirect_server = NULL;
        ESP_LOGE("panel_web", "HTTP 80 redirect not started (HTTPS 443 unaffected)");
        return;
    }
    ESP_LOGI("panel_web", "HTTP 80 redirects to HTTPS 443");
}
/* On the HTTPS worker (httpd_queue_work): the browser sign-ins' owed flash writes. The worker
 * writes them on every sign-in already; this task's slow stack must not (web_login_retry()). */
static void login_retry_work(void *arg)
{
    (void)arg;
    web_login_retry();
}
static void maintenance(void *arg)
{
    (void)arg;
    unsigned seen = 0;
    unsigned retry_ticks = 0;
    unsigned owed_ticks = 0;
    bool wanted = false; /* the server should run: the Panel Manager is on, or setup needs it */
    for (;;) {
        web_setup_poll();
        const bool want = atomic_load(&web_on) || web_setup_listener();
        if (want != wanted) {
            wanted = want;
            atomic_fetch_add(&generation, 1);
        }
        unsigned gen = atomic_load(&generation);
        if (gen != seen) {
            if (redirect_server) {
                httpd_stop(redirect_server);
                redirect_server = NULL;
            }
            if (server) {
                voice_drain(); /* an upload's task holds a request of this server */
                web_screen_stop(); /* nothing of the remote keypad's reaches it any more */
                httpd_ssl_stop(server);
                server = NULL;
                web_screen_stopped();
                voice_drain_end();
            }
            release_control();
            mbedtls_platform_zeroize(sessions, sizeof(sessions));
            if (wanted) {
                httpd_ssl_config_t c = HTTPD_SSL_CONFIG_DEFAULT();
                c.port_secure = 443;
                c.servercert = cert;
                c.servercert_len = cert_len;
                c.prvtkey_pem = key;
                c.prvtkey_len = key_len;
                c.httpd.ctrl_port = 32770; /* HTTPS default 32769 belongs to dev_remote */
                c.httpd.max_open_sockets = 4; /* the remote keypad's WebSocket and the pages */
                c.httpd.max_uri_handlers = WEB_URI_HANDLERS;
                c.httpd.stack_size = 16384;
                c.httpd.task_priority = 2;
                c.httpd.open_fn = low_latency_socket;
                c.httpd.close_fn = socket_closed;
                c.httpd.recv_wait_timeout = 3;
                c.httpd.send_wait_timeout = 3;
                c.tls_handshake_timeout_ms = 5000;
                c.httpd.uri_match_fn = httpd_uri_match_wildcard;
                c.httpd.lru_purge_enable = true;
                esp_err_t e = httpd_ssl_start(&server, &c);
                if (e == ESP_OK) {
                    /* First match wins: specific API routes, then the API, then files. */
                    web_screen_register(server);
                    route("/api/v1/login", HTTP_POST, login);
                    route("/api/v1/network-confirm", HTTP_POST, continuity);
                    route("/api/v1/update-confirm", HTTP_POST, continuity);
                    route("/api/v1/upload", HTTP_PUT, upload);
                    route("/api/v1/zwave-radio", HTTP_PUT, zwave_radio_put);
                    route("/api/v1/backup", HTTP_POST, backup_post);
                    route("/api/v1/backup", HTTP_PUT, backup_put);
                    route("/api/v1/languages", HTTP_PUT, languages_put);
                    route("/api/v1/voices", HTTP_PUT, voices_put);
                    route("/api/v1/*", HTTP_GET, get);
                    route("/api/v1/*", HTTP_POST, post);
                    route("/*", HTTP_GET, assets);
                    ESP_LOGI("panel_web", "Panel Manager ready on HTTPS 443");
                    redirect_start(); /* only beside a running HTTPS server */
                } else
                    ESP_LOGE("panel_web", "HTTPS start failed: %s", esp_err_to_name(e));
            }
            seen = gen;
        }
        if (!server && wanted && ++retry_ticks % 5 == 0)
            atomic_fetch_add(&generation, 1);
        /* A sign-in turned off that flash refused: retried every 5 s with nobody at the browser
         * (web_login_retry() itself waits its own 5 s between flash tries; code review S01). */
        if (server && web_login_owed() && ++owed_ticks % 5 == 0)
            (void)httpd_queue_work(server, login_retry_work, NULL);
        if (server)
            (void)httpd_queue_work(server, web_keypads_tick, NULL);
        else
            web_keypads_tick(NULL); /* server stopped: no HTTP worker races this expiry */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#if CONFIG_ALARM_SERVICE_CONSOLE
static int console(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "on"))
        return panel_web_set(true, atomic_load(&dev_on)) == ESP_OK ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "off"))
        return panel_web_set(false, false) == ESP_OK ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "developer"))
        return panel_web_set(true, !strcmp(argv[2], "on")) == ESP_OK ? 0 : 1;
    if (argc >= 3 && !strcmp(argv[1], "provision"))
        return web_setup_provision(argc - 2, argv + 2); /* setup over USB (web_setup_usb.c) */
    if (argc == 2 && !strcmp(argv[1], "setup")) {
        web_setup_print_label(); /* the physical service console only */
        return 0;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "password")) {
        /* The owner's way in from away: a temporary password to be changed at first sign-in.
         * Physical service console only (not in release firmware). */
        const int user = atoi(argv[2]);
        if (user < 1 || user > ALARM_MAX_USERS) {
            printf("WEB password: user 1..%d\n", ALARM_MAX_USERS);
            return 1;
        }
        if (argc == 4 && !strcmp(argv[3], "off")) {
            if (!web_login_clear((uint8_t)user)) {
                printf("WEB password: user %d browser sign-in off until a restart: flash refused "
                       "it, retrying\n", user);
                return 1;
            }
            printf("WEB password: user %d browser sign-in off\n", user);
            return 0;
        }
        char pw[24], why[96];
        if (!web_login_set_temporary((uint8_t)user, pw, sizeof(pw), why, sizeof(why))) {
            printf("WEB password: %s\n", why);
            return 1;
        }
        printf("WEB password: user %d temporary password %s (must be changed at sign-in)\n",
               user, pw);
        mbedtls_platform_zeroize(pw, sizeof(pw));
        return 0;
    }
    printf("WEB enabled=%d developer=%d HTTPS=443 HTTP80=%s ID=%s\n", atomic_load(&web_on),
           atomic_load(&dev_on), redirect_server ? "redirect" : "off", identity);
    uint8_t hash[32];
    char fingerprint[65];
    if (digest(cert, cert_len, hash)) {
        hex(hash, 32, fingerprint);
        printf("WEB certificate SHA256 %s\n", fingerprint);
    }
    return 0;
}
#endif
/* The Panel Manager on, for good, without restarting the server that setup is using
 * (web_setup.c, once its accounts exist). */
static esp_err_t web_on_quietly(void)
{
    xSemaphoreTake(settings_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t e = nvs_open("panelweb", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, "mode", (uint8_t)(1u | (atomic_load(&dev_on) ? 2u : 0u)));
        if (e == ESP_OK)
            e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK)
        atomic_store(&web_on, true);
    xSemaphoreGive(settings_lock);
    return e;
}
void panel_web_setup_button(bool (*down)(void))
{
    web_setup_button(down);
}
static void *json_alloc(size_t size)
{
    return alarm_mem_slow_malloc(size);
}
esp_err_t panel_web_start(const dev_remote_adapter_t *a)
{
    A = *a;
    esp_err_t screen = web_screen_setup(a);
    if (screen != ESP_OK)
        ESP_LOGE("panel_web", "remote keypad unavailable: %s", esp_err_to_name(screen));
    ui_port_esp_backend(&B);
    /* A configuration reply is a tree of ~1,500 small nodes; with plain malloc every one of
     * them (under the 4 KiB PSRAM threshold) landed in internal RAM. This component is the
     * only cJSON user in the firmware, so the process-wide hooks are ours to set. */
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    hex(esp_app_get_description()->app_elf_sha256, 8, asset_tag + 1);
    asset_tag[0] = '"';
    snprintf(asset_tag_plain, sizeof(asset_tag_plain), "%sp\"", asset_tag);
    strcat(asset_tag, "\"");
    settings_lock = xSemaphoreCreateMutex();
    s_serve_lock = xSemaphoreCreateRecursiveMutex();
    if (!settings_lock || !s_serve_lock)
        return ESP_ERR_NO_MEM;
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK)
        return ESP_FAIL;
    hex(mac, 6, identity);
    random_token(boot);
    nvs_handle_t h;
    esp_err_t e = nvs_open("panelweb", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    uint8_t mode = 0;
    e = nvs_get_u8(h, "mode", &mode);
    if (e == ESP_ERR_NVS_NOT_FOUND) { /* Keep existing explicitly enrolled dev panels reachable
                                    through this upgrade. Fresh panels remain off. */
        nvs_handle_t old;
        if (nvs_open("devrem", NVS_READONLY, &old) == ESP_OK) {
            size_t len = 0;
            if (nvs_get_blob(old, "peer", NULL, &len) == ESP_OK && len > 0)
                mode = 3;
            nvs_close(old);
        }
        e = nvs_set_u8(h, "mode", mode);
        if (e == ESP_OK)
            e = nvs_commit(h);
    }
    if (e != ESP_OK || mode > 3 || mode == 2) {
        nvs_close(h);
        return ESP_ERR_INVALID_STATE;
    }
    e = dev_cert_ensure(h, identity, cert, &cert_len, key, &key_len);
    nvs_close(h);
    if (e != ESP_OK)
        return e;
    atomic_store(&web_on, (mode & 1) != 0);
    atomic_store(&dev_on, (mode & 3) == 3);
    dev_remote_set_allowed(atomic_load(&dev_on));
    atomic_store(&generation, 1);
    /* A panel without a screen is set up from a phone (web_setup.c); a panel with one keeps the
     * Panel Manager off until someone turns it on at the screen. */
    uint8_t cert_hash[32];
    char fingerprint[65] = "";
    if (digest(cert, cert_len, cert_hash))
        hex(cert_hash, 32, fingerprint);
#if CONFIG_ALARM_FEATURE_UI
    const bool headless_master = false;
#elif CONFIG_ALARM_LINK
    const bool headless_master = plink_service_role() == PLINK_SERVICE_MASTER;
#else
    const bool headless_master = true;
#endif
    s_setup_board = headless_master;
    e = web_setup_init(headless_master, fingerprint, web_on_quietly);
    if (e != ESP_OK)
        ESP_LOGE("panel_web", "headless setup unavailable: %s", esp_err_to_name(e));
    /* Once at start, on this task (it writes flash above): the stored sign-ins in line with the
     * users - a deleted user's that flash wouldn't erase before the restart goes now. */
    web_login_retry();
#if CONFIG_ALARM_SERVICE_CONSOLE
    const esp_console_cmd_t cmd = {
        .command = "web",
        .help = "Panel Manager status/on/off/developer on|off, password <user> [off] "
                "(a temporary browser password; physical service console), setup (a headless "
                "panel's setup label)",
        .func = console};
    esp_console_cmd_register(&cmd);
#endif
    /* The maintenance task only starts and stops the server (no flash writes): slow stack. */
    return alarm_mem_task_create(maintenance, "panel_web", 6144, NULL, 1, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
